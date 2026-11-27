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

## Where it faults (symbolized)

```
* thread #9, name = 'frida-agent-con', stop reason = EXC_BAD_ACCESS
  frame #0: libsystem_pthread`pthread_rwlock_rdlock + 8
  frame #1: frida-agent.dylib`g_type_from_name(name="GTlsBackendOpenssl")   gtype.c:3222
  frame #2: frida-agent.dylib`g_type_register_static("GTlsBackendOpenssl")  gtype.c:2572
  frame #4: frida-agent.dylib`g_tls_backend_openssl_register_type
  frame #6: frida-agent.dylib`_frida_agent_environment_init                 agent-glue.c:77
  frame #7: frida-agent.dylib`frida_agent_runner_create_and_run             agent.vala:124
  frame #8: two-managers`frida_agent_container_run                         agent-container.vala:119
```

The second agent container re-runs `_frida_agent_environment_init` (gio_init,
the OpenSSL GIO module registration) into a GLib whose type system was already
torn down by the first container's deinit: `g_type_register_static` reaches
`g_type_from_name` and takes a type-system rwlock that is no longer mapped.

## Tried and eliminated
- revert `a0b9b89a` (map the agent from memory): still faults.
- frida-gum at 17.27.5: still faults.
- `fa943cb3` (unload the agent before joining its thread): still faults.
- init-once guard in `_frida_agent_environment_init` for all platforms, plus a
  no-op `_frida_agent_environment_deinit`: still faults — so it is not the
  re-init of one image, it is the next image meeting a GLib the previous one
  shut down.

## What is left
Decide the lifetime: either the agent stays loaded for the process (and its
GLib is never shut down between sessions), or each session gets an image of its
own, as 17.27.5 did with a per-process temp directory — which is exactly what
`2dd9ebd2` replaced with a shared resource directory.

## The fix

`AgentResource` takes the session's temporary directory again, so the agent is
written per session and each session loads its own image; the resource
directory still holds the **helper**, which is stateless and shared safely.

That is the lifetime rule the crash was breaking: an image whose GLib is shut
down must not be reused. 17.27.5 got it for free from a per-process temp
directory; `2dd9ebd2` moved the agent to the shared directory and kept the
shutdown, so the next session's `gio_init`/`g_io_module_openssl_register` ran
against a torn-down type system.

With the fix, the harness runs three rounds without a fault (the exit still
hangs in `frida_deinit()`, which 17.27.5's tree does too — unrelated).
