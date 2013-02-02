/*
 * Copyright 2013 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "folly/experimental/io/AsyncIO.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>

#include <cstdlib>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

#include <glog/logging.h>
#include <gtest/gtest.h>

#include "folly/experimental/io/FsUtil.h"
#include "folly/ScopeGuard.h"
#include "folly/String.h"

namespace fs = folly::fs;
using folly::AsyncIO;

namespace {

constexpr size_t kAlignment = 512;  // align reads to 512 B (for O_DIRECT)

struct TestSpec {
  off_t start;
  size_t size;
};

void waitUntilReadable(int fd) {
  pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;

  int r;
  do {
    r = poll(&pfd, 1, -1);  // wait forever
  } while (r == -1 && errno == EINTR);
  PCHECK(r == 1);
  CHECK_EQ(pfd.revents, POLLIN);  // no errors etc
}

folly::Range<AsyncIO::Op**> readerWait(AsyncIO* reader) {
  int fd = reader->pollFd();
  if (fd == -1) {
    return reader->wait(1);
  } else {
    waitUntilReadable(fd);
    return reader->pollCompleted();
  }
}

// Temporary file that is NOT kept open but is deleted on exit.
// Generate random-looking but reproduceable data.
class TemporaryFile {
 public:
  explicit TemporaryFile(size_t size);
  ~TemporaryFile();

  const fs::path path() const { return path_; }

 private:
  fs::path path_;
};

TemporaryFile::TemporaryFile(size_t size)
  : path_(fs::temp_directory_path() / fs::unique_path()) {
  CHECK_EQ(size % sizeof(uint32_t), 0);
  size /= sizeof(uint32_t);
  const uint32_t seed = 42;
  std::mt19937 rnd(seed);

  const size_t bufferSize = 1U << 16;
  uint32_t buffer[bufferSize];

  FILE* fp = ::fopen(path_.c_str(), "wb");
  PCHECK(fp != nullptr);
  while (size) {
    size_t n = std::min(size, bufferSize);
    for (size_t i = 0; i < n; ++i) {
      buffer[i] = rnd();
    }
    size_t written = ::fwrite(buffer, sizeof(uint32_t), n, fp);
    PCHECK(written == n);
    size -= written;
  }
  PCHECK(::fclose(fp) == 0);
}

TemporaryFile::~TemporaryFile() {
  try {
    fs::remove(path_);
  } catch (const fs::filesystem_error& e) {
    LOG(ERROR) << "fs::remove: " << folly::exceptionStr(e);
  }
}

TemporaryFile thisBinary(6 << 20);  // 6MiB

void testReadsSerially(const std::vector<TestSpec>& specs,
                       AsyncIO::PollMode pollMode) {
  AsyncIO aioReader(1, pollMode);
  AsyncIO::Op op;
  int fd = ::open(thisBinary.path().c_str(), O_DIRECT | O_RDONLY);
  PCHECK(fd != -1);
  SCOPE_EXIT {
    ::close(fd);
  };

  for (int i = 0; i < specs.size(); i++) {
    std::unique_ptr<char[]> buf(new char[specs[i].size]);
    aioReader.pread(&op, fd, buf.get(), specs[i].size, specs[i].start);
    EXPECT_EQ(aioReader.pending(), 1);
    auto ops = readerWait(&aioReader);
    EXPECT_EQ(1, ops.size());
    EXPECT_TRUE(ops[0] == &op);
    EXPECT_EQ(aioReader.pending(), 0);
    ssize_t res = op.result();
    EXPECT_LE(0, res) << folly::errnoStr(-res);
    EXPECT_EQ(specs[i].size, res);
    op.reset();
  }
}

void testReadsParallel(const std::vector<TestSpec>& specs,
                       AsyncIO::PollMode pollMode) {
  AsyncIO aioReader(specs.size(), pollMode);
  std::unique_ptr<AsyncIO::Op[]> ops(new AsyncIO::Op[specs.size()]);
  std::vector<std::unique_ptr<char[]>> bufs(specs.size());

  int fd = ::open(thisBinary.path().c_str(), O_DIRECT | O_RDONLY);
  PCHECK(fd != -1);
  SCOPE_EXIT {
    ::close(fd);
  };
  for (int i = 0; i < specs.size(); i++) {
    bufs[i].reset(new char[specs[i].size]);
    aioReader.pread(&ops[i], fd, bufs[i].get(), specs[i].size,
                    specs[i].start);
  }
  std::vector<bool> pending(specs.size(), true);

  size_t remaining = specs.size();
  while (remaining != 0) {
    EXPECT_EQ(remaining, aioReader.pending());
    auto completed = readerWait(&aioReader);
    size_t nrRead = completed.size();
    EXPECT_NE(nrRead, 0);
    remaining -= nrRead;

    for (int i = 0; i < nrRead; i++) {
      int id = completed[i] - ops.get();
      EXPECT_GE(id, 0);
      EXPECT_LT(id, specs.size());
      EXPECT_TRUE(pending[id]);
      pending[id] = false;
      ssize_t res = ops[id].result();
      EXPECT_LE(0, res) << folly::errnoStr(-res);
      EXPECT_EQ(specs[id].size, res);
    }
  }
  EXPECT_EQ(aioReader.pending(), 0);
  for (int i = 0; i < pending.size(); i++) {
    EXPECT_FALSE(pending[i]);
  }
}

void testReads(const std::vector<TestSpec>& specs,
               AsyncIO::PollMode pollMode) {
  testReadsSerially(specs, pollMode);
  testReadsParallel(specs, pollMode);
}

}  // anonymous namespace

TEST(AsyncIO, ZeroAsyncDataNotPollable) {
  testReads({{0, 0}}, AsyncIO::NOT_POLLABLE);
}

TEST(AsyncIO, ZeroAsyncDataPollable) {
  testReads({{0, 0}}, AsyncIO::POLLABLE);
}

TEST(AsyncIO, SingleAsyncDataNotPollable) {
  testReads({{0, 512}}, AsyncIO::NOT_POLLABLE);
  testReads({{0, 512}}, AsyncIO::NOT_POLLABLE);
}

TEST(AsyncIO, SingleAsyncDataPollable) {
  testReads({{0, 512}}, AsyncIO::POLLABLE);
  testReads({{0, 512}}, AsyncIO::POLLABLE);
}

TEST(AsyncIO, MultipleAsyncDataNotPollable) {
  testReads({{512, 1024}, {512, 1024}, {512, 2048}}, AsyncIO::NOT_POLLABLE);
  testReads({{512, 1024}, {512, 1024}, {512, 2048}}, AsyncIO::NOT_POLLABLE);

  testReads({
    {0, 5*1024*1024},
    {512, 5*1024*1024},
  }, AsyncIO::NOT_POLLABLE);

  testReads({
    {512, 0},
    {512, 512},
    {512, 1024},
    {512, 10*1024},
    {512, 1024*1024},
  }, AsyncIO::NOT_POLLABLE);
}

TEST(AsyncIO, MultipleAsyncDataPollable) {
  testReads({{512, 1024}, {512, 1024}, {512, 2048}}, AsyncIO::POLLABLE);
  testReads({{512, 1024}, {512, 1024}, {512, 2048}}, AsyncIO::POLLABLE);

  testReads({
    {0, 5*1024*1024},
    {512, 5*1024*1024},
  }, AsyncIO::POLLABLE);

  testReads({
    {512, 0},
    {512, 512},
    {512, 1024},
    {512, 10*1024},
    {512, 1024*1024},
  }, AsyncIO::POLLABLE);
}

TEST(AsyncIO, ManyAsyncDataNotPollable) {
  {
    std::vector<TestSpec> v;
    for (int i = 0; i < 1000; i++) {
      v.push_back({512 * i, 512});
    }
    testReads(v, AsyncIO::NOT_POLLABLE);
  }
}

TEST(AsyncIO, ManyAsyncDataPollable) {
  {
    std::vector<TestSpec> v;
    for (int i = 0; i < 1000; i++) {
      v.push_back({512 * i, 512});
    }
    testReads(v, AsyncIO::POLLABLE);
  }
}

TEST(AsyncIO, NonBlockingWait) {
  AsyncIO aioReader(1, AsyncIO::NOT_POLLABLE);
  AsyncIO::Op op;
  int fd = ::open(thisBinary.path().c_str(), O_DIRECT | O_RDONLY);
  PCHECK(fd != -1);
  SCOPE_EXIT {
    ::close(fd);
  };
  size_t size = 1024;
  std::unique_ptr<char[]> buf(new char[size]);
  aioReader.pread(&op, fd, buf.get(), size, 0);
  EXPECT_EQ(aioReader.pending(), 1);

  folly::Range<AsyncIO::Op**> completed;
  while (completed.empty()) {
    // poll without blocking until the read request completes.
    completed = aioReader.wait(0);
  }
  EXPECT_EQ(completed.size(), 1);

  EXPECT_TRUE(completed[0] == &op);
  ssize_t res = op.result();
  EXPECT_LE(0, res) << folly::errnoStr(-res);
  EXPECT_EQ(size, res);
  EXPECT_EQ(aioReader.pending(), 0);
}
