# Diagnostics Viewer

A C++17 profiler for measuring blocks of work, with a separate Python viewer.
Use it to view timings, numeric values and logs from any C++ application.
The library has no external framework dependencies.

## Start the viewer

The viewer needs Python 3.9+ and a browser. No pip packages are required.

```powershell
python Tools/diagnostics_viewer.py
```

Choose a session from the dropdown when several applications are running.
Block scopes start collapsed. 
Press Ctrl+C in the terminal to stop the viewer. Add `--terminal` for a terminal
display, or `--help` to see other options.

Session files are saved in the system temporary directory's `Diagnostics` folder.
You can delete old files when they are no longer needed.

## Add to your project

Use CMake 3.22+ and a C++17 compiler. For a local checkout, add this to your
application's `CMakeLists.txt`:

```cmake
add_subdirectory("../diagnostics-viewer" diagnostics-viewer)
target_link_libraries(MyApplication PRIVATE Diagnostics::Diagnostics)
```

This assumes the `diagnostics-viewer` repository folder is beside your application.
Adjust the path if you store it elsewhere.

If Python is available when configuring the project, you can also start the
viewer through CMake:

```powershell
cmake --build build --config RelWithDebInfo --target diagnostics_viewer
```

## Measure a block

Create a session before processing starts. Names in `scopes` and `values` are
assigned IDs in list order, starting at zero.

```cpp
#include <diagnostics/Diagnostics.h>
#include <utility>

diagnostics::Config config;
config.application = "MyApplication";
config.scopes = { "block", "process", "filter" }; // IDs 0, 1, 2
config.values = { "active_tasks" };               // ID  0

diagnostics::Session session(std::move(config));
```

Inside each processing callback or frame:

```cpp
diagnostics::BlockInfo info;
info.budgetMs = budgetMilliseconds;

diagnostics::Block block(session, info); // scope 0: the whole block
{
    diagnostics::Scope process(block, 1);
    {
        diagnostics::Scope filter(block, 2);
        applyFilter();
    }
    processOtherWork();
}
block.setValue(0, activeTasks);
```

Set scope and value names before processing starts, with up to eight of each.
The whole block uses one scope slot, leaving seven names for inner scopes.
At most eight scopes can be active at once, including the whole block; attempts
to exceed either limit are ignored. Any inner scope can contain further scopes.
Use one block at a time per session, and end inner scopes before outer scopes.
Create and destroy sessions outside realtime callbacks; stop callbacks before
destroying their session.

## Read the timings

<p align="center">
  <img
    src="img/diagnostics001.png"
    width="497"
    style="max-width: 100%; height: auto;"
  />
</p>

Scope zero covers the whole block. Other scopes measure work inside it.
**Total** includes child scopes; **self** subtracts their time. For example:

```text
block (scope 0): 10 ms total
└── process (scope 1): 7 ms total
    └── filter (scope 2): 2 ms total
```

| Scope | Total time | Self time |
| --- | --- | --- |
| `block` | 10 ms | 10 - 7 = **3 ms** |
| `process` | 7 ms | 7 - 2 = **5 ms** |
| `filter` | 2 ms | **2 ms** (no child scopes) |

The 7 ms in `process` already includes the 2 ms in `filter`. Work without its own
scope counts toward self time. Timings and call counts are averaged per block.
The viewer uses the recorded parent relationships to display nested scopes.
Only work instrumented with `diagnostics::Scope` appears as a separate branch.
If a scope name is used under multiple parents, its combined timings appear at
the top level. Older recordings show all inner scopes directly under the block;
rebuild the instrumented application to record the full hierarchy.

**Load** shows how much of a block's time budget was used. A block taking 2 ms
with a 4 ms budget has 50% load. Above 100% means it took longer than allowed.

For audio, set `budgetMs` to `1000 * actualBlockSize / sampleRate`. For rendering,
use the time allowed for one frame in milliseconds. Average load compares total
block time with total budget time, so changing block sizes is supported.

The graph shows average and peak (highest) load; the red line marks 100%.S
**P99** is a block time that about 99% of sampled blocks stay below. Peak and P99
cover the latest update. The status bar keeps the session's highest block time
and total count of over-budget blocks. For audio, this count cannot tell you how
many device dropouts occurred.

<p align="center">
  <img
    src="img/diagnostics002.png"
    width="497"
    style="max-width: 100%; height: auto;"
  />
</p>

Dropped blocks or logs mean some data is missing. For useful comparisons, use
the same machine and workload; for audio, keep sample rate and buffer size the same.

## Logs

Log messages to a session with:

```cpp
session.log("Started");
session.log("Something went wrong", "warning");
```

Keep logging and printing outside realtime callbacks. Use `block.setValue()`
to report numeric values from a callback.

To also capture `std::cout` and `std::cerr`, set this before creating the session:

```cpp
config.captureStandardStreams = true;
```

Output still goes to its original destination. Captured output is shared by all
sessions using capture and excludes `printf` and debugger output. Avoid using
other stream-redirection systems at the same time.

## Run the tests

```powershell
cmake -S . -B build-diagnostics-tests -DDIAGNOSTICS_BUILD_TESTS=ON
cmake --build build-diagnostics-tests --config Debug
ctest --test-dir build-diagnostics-tests -C Debug --output-on-failure
```
