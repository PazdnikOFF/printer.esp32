#pragma once
#include <stdbool.h>

/*
 * Sensor state API — called by the external print module when physical
 * sensor inputs change.  This module translates sensor states into the
 * appropriate sony898_state_t automatically.
 *
 * Error priority (highest → lowest):
 *   system_error > cover_open > no_ribbon > no_paper > no_media > media_mismatch
 *
 * Errors take immediate effect regardless of current print state.
 * When all errors are cleared the printer returns to IDLE (interrupted
 * jobs must be restarted by the host).
 *
 * All functions are safe to call from any task context.
 */

void sony898_sensor_init(void);

/* true = paper present in tray */
void sony898_sensor_set_paper(bool present);

/* true = cover is closed (no mechanical fault) */
void sony898_sensor_set_cover(bool closed);

/* true = ribbon cassette is installed */
void sony898_sensor_set_ribbon(bool present);

/* true = installed media type matches the pending job */
void sony898_sensor_set_media_match(bool matches);

/* true = hardware / system fault (overtemp, motor error, etc.) */
void sony898_sensor_set_system_error(bool error);

/* Clear all error flags and return to IDLE.  Use after servicing a fault. */
void sony898_sensor_clear_all(void);

/* Returns true if any sensor is currently reporting an error. */
bool sony898_sensor_has_error(void);
