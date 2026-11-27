# A second DeviceManager after a script was loaded into this process

At 17.28.0 this faults (SIGSEGV, exit 139) on the second round, in the agent
container thread: `bt` shows `frida_agent_container_run` → frida-agent.dylib →
`pthread_rwlock_rdlock` on an address that is no longer mapped.

It is not frida-rust: the same loop through frida-python 17.28.0 faults the same
way, and 17.27.5 runs it three times without a fault.

```c
// repro/two-managers.c — round: new DeviceManager → local device → attach(0)
//   → create_script("1;") → load → unref → detach → close → unref
```

Built against this tree (the devkit the build makes):

```
ninja -C build-macos
cd build-macos/src/devkit
clang two-managers.c -o two-managers -I. \
  $(pkg-config --cflags glib-2.0 gio-2.0 json-glib-1.0) -L. -lfrida-core \
  -lbsm -ldl -lresolv -lm $(pkg-config --libs glib-2.0 gio-2.0 json-glib-1.0) \
  -Wl,-framework,Foundation,-framework,CoreFoundation,-framework,AppKit,-framework,CoreServices
cp ../frida-helper ../lib/agent/frida-agent.dylib .
./two-managers
```

## Bisect (same harness, this tree, macOS arm64)

| commit | result |
|---|---|
| 17.27.5 (release) | three rounds, no fault |
| `4a421169` (before the resource-directory work) | three rounds, no fault |
| `c90e78ac` (system: self-reclaiming resource directories) | three rounds, no fault |
| **`2dd9ebd2` (darwin: keep the helper and agent in resource dirs)** | **faults on round 1** |
| 17.28.0 | faults on round 1 |
| 17.28.0 with `a0b9b89a` (darwin: map the agent from memory) reverted | still faults |
| 17.28.0 with frida-gum at 17.27.5 | still faults |

So the regression is in `2dd9ebd2`: the agent's path moved to a resource
directory, and a second `DeviceManager` in the same process ends up with an
agent it cannot share — the likely shape is a second copy of the agent image
(each `provide()` that rewrites the file gives a new inode, so `dlopen` maps a
second agent rather than returning the loaded one).

The fix belongs here, not in the bindings. The harness above is the test.
