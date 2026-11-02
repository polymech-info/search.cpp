# Build a cross-platform C++20 local file search / grep engine

## Targets

- Windows
- macOS
- Linux

Constraints:

- C++20
- No mandatory third-party libraries
- No index
- Local filesystem only
- Fast literal search first
- Regex optional later
- Must support large files via streaming
- Must be usable from a desktop GUI app

Architecture:

```txt
search-core/
  include/
    search/SearchEngine.hpp
    search/FileWalker.hpp
    search/FileReader.hpp
    search/Matcher.hpp
    search/SearchTypes.hpp
    search/IgnoreRules.hpp
    search/ThreadPool.hpp

  src/
    SearchEngine.cpp
    Matcher.cpp
    IgnoreRules.cpp
    ThreadPool.cpp

    platform/
      win/FileWalkerWin.cpp
      win/FileReaderWin.cpp
      posix/FileWalkerPosix.cpp
      posix/FileReaderPosix.cpp
````

Core abstractions:

```cpp
struct SearchOptions {
    std::vector<std::filesystem::path> roots;

    std::string needle;
    bool caseSensitive = true;
    bool wholeWord = false;

    bool includeHidden = false;
    bool followSymlinks = false;
    bool skipBinary = true;

    uint64_t maxFileSizeBytes = 256ull * 1024 * 1024;
    uint32_t maxMatchesPerFile = 1000;

    size_t chunkSize = 4 * 1024 * 1024;
    size_t workerCount = std::thread::hardware_concurrency();

    std::vector<std::string> excludeDirNames = {
        ".git", "node_modules", "dist", "build", "target", "bin", "obj", ".cache"
    };

    std::vector<std::string> includeExtensions;
    std::vector<std::string> excludeExtensions;
};

struct SearchMatch {
    std::filesystem::path path;
    uint64_t byteOffset = 0;
    uint64_t line = 0;
    uint64_t column = 0;
    std::string linePreview;
};

struct SearchStats {
    uint64_t filesVisited = 0;
    uint64_t filesScanned = 0;
    uint64_t filesSkipped = 0;
    uint64_t bytesScanned = 0;
    uint64_t matches = 0;
};

struct SearchCallbacks {
    std::function<void(const SearchMatch&)> onMatch;
    std::function<void(const SearchStats&)> onProgress;
    std::function<bool()> isCancelled;
};
```

Public API:

```cpp
class SearchEngine {
public:
    void search(const SearchOptions& options, const SearchCallbacks& callbacks);
};
```

Platform abstraction:

```cpp
class IFileWalker {
public:
    virtual ~IFileWalker() = default;

    virtual void walk(
        const std::filesystem::path& root,
        const SearchOptions& options,
        const std::function<void(const std::filesystem::path&, uint64_t size)>& onFile,
        const std::function<bool()>& isCancelled
    ) = 0;
};

class IFileReader {
public:
    virtual ~IFileReader() = default;

    virtual bool scanFile(
        const std::filesystem::path& path,
        uint64_t size,
        const SearchOptions& options,
        Matcher& matcher,
        const SearchCallbacks& callbacks,
        SearchStats& stats
    ) = 0;
};
```

Windows implementation:

```txt
Traversal:
  FindFirstFileExW
  FindNextFileW
  FindExInfoBasic
  FIND_FIRST_EX_LARGE_FETCH

Reading:
  CreateFileW
  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE
  FILE_FLAG_SEQUENTIAL_SCAN
  ReadFile
```

macOS/Linux implementation:

```txt
Traversal:
  fts_open / fts_read / fts_close
  or opendir / readdir fallback

Reading:
  open
  read
  close

Optional hints:
  posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL)
```

Large file strategy:

```txt
Never load huge files fully.

For each file:
  if size < 1 MB:
    read whole file
  else:
    stream in chunks

Default chunk size:
  4 MB

Boundary handling:
  keep overlap = needle.size() - 1 bytes
  prepend carry bytes to next chunk
  scan carry + new chunk
```

Pseudo:

```cpp
carry = 0;

while (read chunk) {
    total = carry + bytesRead;

    scan(buffer, total);

    carry = min(needle.size() - 1, total);
    memmove(buffer, buffer + total - carry, carry);
}
```

Binary detection:

```txt
Read first 4–8 KB.
If buffer contains NUL byte:
  treat as binary.
Skip binary files by default.
```

Matching:

```txt
v0:
  literal byte search

v1:
  case-insensitive ASCII search
  whole-word mode
  Boyer-Moore-Horspool for longer needles

v2 optional:
  PCRE2 or RE2 behind compile flag
```

Do not use:

```txt
std::regex
std::ifstream for hot path
std::getline for hot path
recursive async spawning
one thread per file
mmap as default
```

Threading model:

```txt
1 traversal stage
N file scanning workers
bounded queue between them
cancellation checked frequently
results emitted incrementally
```

Important UX requirements:

```txt
- return matches incrementally
- allow cancellation
- keep UI thread untouched
- cap matches per file
- cap total preview size
- expose progress stats
- skip permission errors silently but count them
```

Line/column handling:

```txt
Fast mode:
  report byte offset only

Normal mode:
  count newlines while scanning
  keep current line number
  compute column relative to last line start
  extract small preview around the match
```

Path handling:

```txt
Use std::filesystem::path at API boundary.

Windows:
  internally use UTF-16 Win32 APIs.

macOS/Linux:
  internally treat paths as UTF-8 byte paths.
```

Ignore rules:

```txt
Start simple:
  exclude exact directory names
  exclude extensions
  include extensions

Later:
  add .gitignore-like parser
```

Default skipped dirs:

```txt
.git
.hg
.svn
node_modules
dist
build
target
bin
obj
.cache
.next
.nuxt
vendor
```

Default skipped extensions:

```txt
.exe
.dll
.lib
.obj
.pdb
.so
.dylib
.a
.o
.png
.jpg
.jpeg
.webp
.gif
.mp4
.mov
avi
zip
7z
rar
tar
gz
sqlite
db
pdf
```

Performance priorities:

```txt
1. avoid allocations in hot loop
2. scan raw bytes
3. avoid UTF conversion during scan
4. use large read buffers
5. skip garbage early
6. stream results
7. keep queues bounded
8. avoid flooding UI with too many callbacks
```

Deliverables:

```txt
1. Header-only public API
2. Windows walker/reader implementation
3. POSIX walker/reader implementation
4. Literal matcher with chunk boundary support
5. Threaded SearchEngine
6. Small CLI test app:
     pmgrep "needle" ./folder
7. Unit tests for:
     - chunk boundary matches
     - binary detection
     - case sensitivity
     - skipped dirs
     - max matches per file
     - cancellation
```

Goal:

Build a small, dependency-free, ripgrep-ish local search core suitable for embedding in a C++ desktop app.

```
```
````markdown
## Threading interface

Use a simple job system with bounded queues.

```txt
main/UI thread
  |
  | start SearchEngine::start()
  v
search coordinator thread
  |
  +-- directory walker
  |
  +-- file scan worker 1
  +-- file scan worker 2
  +-- file scan worker N
  |
  +-- result batching
````

Public API:

```cpp
class SearchHandle {
public:
    void cancel();
    bool isRunning() const;
    SearchStats stats() const;
    void wait();
};

class SearchEngine {
public:
    SearchHandle start(
        SearchOptions options,
        SearchCallbacks callbacks
    );
};
```

Callbacks must never run on the UI thread unless explicitly marshalled by the app.

```cpp
struct SearchCallbacks {
    std::function<void(std::vector<SearchMatch>)> onMatchesBatch;
    std::function<void(SearchStats)> onProgress;
    std::function<void(SearchStats)> onFinished;
    std::function<void(std::exception_ptr)> onError;
};
```

Recommended batching:

```txt
worker threads push matches into result queue
coordinator flushes:
  every 16–50 ms
  or when batch reaches e.g. 100 matches
```

Cancellation token:

```cpp
class CancellationToken {
public:
    bool isCancellationRequested() const noexcept;
};

class CancellationSource {
public:
    CancellationToken token() const noexcept;
    void cancel() noexcept;
};
```

Internals:

```cpp
template<class T>
class BlockingQueue {
public:
    bool push(T item);       // false if closed/cancelled
    bool pop(T& out);        // false if closed and empty
    void close();
};
```

Queues:

```cpp
BlockingQueue<std::filesystem::path> dirQueue;
BlockingQueue<FileJob> fileQueue;
BlockingQueue<SearchMatch> resultQueue;
```

Better:

```txt
1 walker/coordinator thread recursively enumerates dirs
N file workers scan files
1 result flusher batches callbacks
```

Avoid:

```txt
one thread per directory
one thread per file
calling UI directly from workers
unbounded queues
```

File job:

```cpp
struct FileJob {
    std::filesystem::path path;
    uint64_t size = 0;
};
```

Thread counts:

```cpp
size_t workerCount =
    options.workerCount
        ? options.workerCount
        : std::max(1u, std::thread::hardware_concurrency() - 1);
```

For HDD/network drives, fewer workers can be faster:

```txt
SSD/NVMe:      4–16 workers
HDD:           1–4 workers
network drive: 1–2 workers
```

Progress updates:

```txt
emit every 100–250 ms
not per file
```

Thread ownership:

```cpp
class SearchTask {
public:
    void start();
    void cancel();
    void join();

private:
    std::jthread coordinator_;
    std::vector<std::jthread> workers_;
    std::jthread resultFlusher_;

    CancellationSource cancel_;
    std::atomic<SearchStatsAtomic> stats_;
};
```

If using C++20, prefer `std::jthread` + `std::stop_token`:

```cpp
std::jthread worker([this](std::stop_token st) {
    while (!st.stop_requested()) {
        FileJob job;
        if (!fileQueue.pop(job))
            break;

        scanFile(job, st);
    }
});
```

Design rule:

```txt
SearchHandle::cancel()
  requests stop
  closes queues
  wakes all workers
  returns quickly
```

`wait()` joins threads.

Destructor behavior:

```cpp
SearchHandle::~SearchHandle() {
    cancel();
    wait();
}
```

or document explicitly if destructor is non-blocking.

For GUI apps, safest:

```txt
SearchHandle destructor cancels
SearchEngine owns shared SearchTask
UI explicitly calls cancel on new search
```

Multiple searches:

```txt
Simple v1:
  one active search per SearchEngine

Better:
  SearchEngine::start returns independent SearchHandle
```

Backpressure:

```txt
fileQueue max size:    1024–8192 jobs
resultQueue max size:  configurable
```

If result queue is full:

```txt
block worker briefly
or drop previews, never drop stats
```

Error handling:

```txt
permission denied:
  count skipped
  continue

fatal setup error:
  onError(exception_ptr)

per-file read errors:
  count skipped
  continue
```

UI integration pattern:

```cpp
auto handle = engine.start(options, {
    .onMatchesBatch = [uiDispatcher](std::vector<SearchMatch> matches) {
        uiDispatcher.post([matches = std::move(matches)] {
            resultsModel.append(matches);
        });
    },
    .onProgress = [uiDispatcher](SearchStats stats) {
        uiDispatcher.post([stats] {
            progressView.update(stats);
        });
    },
    .onFinished = [uiDispatcher](SearchStats stats) {
        uiDispatcher.post([stats] {
            progressView.done(stats);
        });
    }
});
```

Important: callbacks run on search threads. The UI layer must marshal them.

CLI integration can call callbacks directly.

Acceptance tests:

```txt
cancel returns quickly
wait joins all threads
no callback after onFinished
no deadlock when result callback is slow
bounded memory when millions of files are found
new search cancels previous search cleanly
```
