#ifndef UPDATE_APPROVAL_H
#define UPDATE_APPROVAL_H

#include <stdbool.h>

#define UPDATE_APPROVAL_BUTTONS 4

/* True while the badge waits for the button presses. */
bool update_approval_waiting(void);

/* Feed every button change. Returns true if the prompt consumed it. */
bool update_approval_button(unsigned int button, bool pressed);

#endif /* UPDATE_APPROVAL_H */
