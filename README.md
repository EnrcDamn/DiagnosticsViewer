# Diagnostics

A small C++17 block profiler with a separate Python viewer. It supports timed
scopes, numeric session values and logs for any application. The library has no
JUCE or profiler dependency. This is a standalone repository: applications import
the library through CMake and keep only their own instrumentation in their source.
The viewer lives here alongside the library and is shared by all applications.

## Viewer

From this repository, start the viewer before or after starting an instrumented
application:

```powershell
python Tools/diagnostics_viewer.py
```

When imported with CMake, the `diagnostics_viewer` target launches the same viewer
from the dependency's source directory, without copying scripts into your project:

```powershell
cmake --build build --config RelWithDebInfo --target diagnostics_viewer
```

This optional target is available when CMake finds Python 3.9+. If Python was
installed after configuration, run CMake configuration again or launch the script
directly from this repository.

The viewer opens independently of the application. Select a session from the
dropdown when using multiple applications or instances. Application names appear
in session labels; the viewer itself is always named **Diagnostics**. A
closed session switches to a newly created session automatically. Resize the
browser window to keep the compact view beside your application. Python 3.9+
and a modern browser are required; there are no pip packages to install. The viewer server
listens only on `127.0.0.1`; the library does not open any network connection.
Ctrl+C in the launching terminal stops the viewer. `--no-browser` prints the URL
without opening it, and `--port NUMBER` selects a fixed local port.

For a terminal display:

```powershell
python Tools/diagnostics_viewer.py --terminal
```

Each instance writes a JSONL session to the system temporary directory's
`Diagnostics` folder (`$env:TEMP\Diagnostics` on Windows). Session
filenames contain process ID, instance number and creation time. The viewer
retains 30 seconds of graph history and up to 200 log lines; session files stay
available for later inspection. Delete old files manually when no longer needed.
Use `--file PATH` to follow one session or `--directory PATH` for another folder.
An abrupt application exit can leave a file without a `closed` marker; the viewer
reports an inactive writer after three seconds without updates.

## What is measured

Each session defines its own scope names. Scope zero measures the complete
block; nested scopes measure selected functions or sections within it. Keep
scopes around blocks of work when profiling realtime code. **Total** time
includes measured child scopes; **self** time subtracts them. The table shows
milliseconds and call counts averaged per block over the latest reporting window.
Self time still includes any uninstrumented functions called within a scope.

Load is elapsed block time divided by the caller-supplied budget in milliseconds.
For audio processing, use `1000 * actualBlockSize / sampleRate`; for a rendering
loop, use the target frame duration. The average is weighted by block budget,
so varying block sizes are supported.
Values above 100% are preserved. The graph displays average and peak block load;
the red line marks 100%. P99 and peak refer to the latest roughly 250 ms window.
The status bar retains the session maximum and cumulative observed over-budget
count. This count is a local timing diagnostic. For audio applications it does
not report device dropouts; host work also shares the available time. Offline
rendering and debugger pauses can exceed the nominal block budget.

Applications can publish up to eight named numeric values with each report.
The viewer displays their names and values without assuming application-specific
states or playback modes. Configure the values before processing starts.
On Windows, the worker also sends logs to `OutputDebugString`, for DebugView or
a debugger's output window. JUCE `DBG()` messages remain available there; they
are not intercepted by this module's log viewer.

## Logging and standard streams

`Session::log("message", "warning")` records an instance-specific message. It is
for **non-audio threads**: it uses a mutex and copies at most 511 bytes per message.
Do not format strings, log or print from the audio callback. Publish numeric
values through the block instead. JSON strings are UTF-8.

Existing `std::cout` / `std::cerr` can optionally be captured:

```cpp
config.captureStandardStreams = true;
```

This forwards output to its original destination and copies it into the
diagnostics log on newline, flush or a full
511-byte chunk. It captures the C++ stream objects used by the module, not C
`printf`, JUCE `DBG`, or every third-party runtime. Standard streams may be shared
with other components, so capture defaults to false. Captured output is
copied to all participating sessions in the same module and cannot identify which
instance originally printed it. Explicit `Session::log` avoids that ambiguity.
Stream adapters remain installed until module shutdown; closing one application
instance cannot leave them referencing that instance. Use capture only outside
the audio thread and avoid combining it with other stream-redirection systems.

## CMake integration

Fetch a pinned revision from a local Git repository or a remote repository:

```cmake
include(FetchContent)
FetchContent_Declare(diagnostics
    GIT_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../Diagnostics"
    GIT_TAG v0.1.0
)
FetchContent_MakeAvailable(diagnostics)
target_link_libraries(MyApplication PRIVATE Diagnostics::Diagnostics)
```

Replace `GIT_REPOSITORY` with your repository URL when hosted remotely. Pin a full
commit hash for reproducible builds. For local development, pass
`-DFETCHCONTENT_SOURCE_DIR_DIAGNOSTICS=C:/path/to/Diagnostics` when configuring your
application; this uses that working checkout directly instead of fetching a revision.
Alternatively, `add_subdirectory` can point at an external checkout with a separate
binary directory.

The library also supports installation and `find_package`:

```powershell
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=C:/path/to/diagnostics-install
cmake --build build --config RelWithDebInfo
cmake --install build --config RelWithDebInfo
```

Configure a consuming application with
`-DCMAKE_PREFIX_PATH=C:/path/to/diagnostics-install`, then use:

```cmake
find_package(Diagnostics 0.1 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE Diagnostics::Diagnostics)
```

Installed viewer files are in `share/diagnostics/Tools` under that prefix; launch
the Python script there directly. The viewer target is provided by source imports.
The C++ API stays the same for either import method:

Create the session on a control thread before processing starts:

```cpp
#include <diagnostics/Diagnostics.h>
#include <utility>

diagnostics::Config config;
config.application = "MyApplication";
config.scopes = { "block", "work" }; // stable numeric IDs: 0, 1
config.values = { "active_tasks" };
diagnostics::Session session(std::move(config));
```

Around a block of work:

```cpp
diagnostics::BlockInfo info;
info.budgetMs = budgetMilliseconds;
diagnostics::Block block(session, info); // begins scope 0
{
    diagnostics::Scope work(block, 1);
    performWork();
}
block.setValue(0, activeTasks);
```

Use one callback producer per session. Callbacks may migrate between host threads,
but must not overlap for the same session. Scope lifetimes must nest; register up
to eight scope names and eight numeric value names before callbacks start.
Stop callbacks before destroying the session. Its destructor drains pending data
and joins its worker; it must run outside audio processing.

Collection uses a preallocated queue of 1023 samples, lock-free atomic indexes,
fixed scope storage and a monotonic clock. Queue submission follows the measured
callback. The audio thread never waits for the worker: full queues drop records,
and the viewer exposes the cumulative drop count. Lost records also mean missed
over-budget observations. Worker memory is bounded; P99 uses up to the first
16384 observed blocks per window (`p99_samples` in JSON). The separate log queue
holds 128 messages and counts busy/full-queue losses. Session creation opens the
file on the control thread. Report writes, formatting, aggregation and debug
output happen on the worker, normally four times per second.
The measurements include instrumentation overhead; compare builds on the same
machine, block budget and workload. For audio, keep sample rate and buffer size
consistent as well.

This library is active when linked and used. The integrating project controls
which build configurations instantiate a session and include profiling calls.
Use the same `Diagnostics::Diagnostics` target when importing into a plugin,
standalone application or project template.

## Checks

The module tests build independently of JUCE:

```powershell
cmake -S . -B build-diagnostics-tests -DDIAGNOSTICS_BUILD_TESTS=ON
cmake --build build-diagnostics-tests --config Debug
ctest --test-dir build-diagnostics-tests -C Debug --output-on-failure
python -m unittest discover -s Tools -p test_diagnostics_viewer.py
```

These cover nested timing, over-budget measurements above 100%, allocation-free
callback collection, queue overflow accounting, shutdown draining, standard
streams with multiple instances, UTF-8/JSON logs and incremental viewer reads.
