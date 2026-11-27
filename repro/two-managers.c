/* Two DeviceManagers in one process, each attaching to this process and loading
 * a script: the sequence that faults at 17.28.0. */
#include "frida-core.h"
#include <stdio.h>

int main(void) {
  frida_init();
  for (int round = 0; round < 3; round++) {
    GError *error = NULL;
    FridaDeviceManager *manager = frida_device_manager_new();
    FridaDevice *device = frida_device_manager_get_device_by_type_sync(manager,
        FRIDA_DEVICE_TYPE_LOCAL, 1000, NULL, &error);
    if (device == NULL) { printf("no local device: %s\n", error->message); return 1; }
    FridaSession *session = frida_device_attach_sync(device, 0, NULL, NULL, &error);
    if (session == NULL) { printf("attach failed: %s\n", error->message); return 1; }
    FridaScript *script = frida_session_create_script_sync(session, "1;", NULL, NULL, &error);
    if (script == NULL) { printf("script failed: %s\n", error->message); return 1; }
    frida_script_load_sync(script, NULL, &error);
    if (error != NULL) { printf("load failed: %s\n", error->message); return 1; }
    g_object_unref(script);
    frida_session_detach_sync(session, NULL, &error);
    if (error != NULL) { printf("detach failed: %s\n", error->message); return 1; }
    g_object_unref(session);
    g_object_unref(device);
    frida_device_manager_close_sync(manager, NULL, &error);
    if (error != NULL) { printf("close failed: %s\n", error->message); return 1; }
    g_object_unref(manager);
    printf("round %d released\n", round);
    fflush(stdout);
  }
  frida_deinit();
  printf("three rounds, no crash\n");
  return 0;
}
