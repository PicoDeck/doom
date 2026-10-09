// Stubs both builds need: the parts of doomgeneric that were left out
// (dummy.c, i_joystick.c).
// --- Networking / Joystick Stubs ---

typedef int boolean;
#ifndef false
#define false 0
#endif
#ifndef true
#define true 1
#endif

// These are needed because we excluded dummy.c
boolean net_client_connected = false;
boolean drone = false;

// Joystick is in i_joystick.c which we excluded
void I_InitJoystick(void) {}
void I_BindJoystickVariables(void) {}
