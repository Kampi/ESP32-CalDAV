/*
 * caldav_client.h
 *
 *  Copyright (C) Daniel Kampert, 2026
 *  Website: www.kampis-elektroecke.de
 *  File info: CalDAV client implementation.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Errors and commissions should be reported to DanielKampert@kampis-elektroecke.de
 */

#ifndef ESP32_CALDAV_CLIENT_H_
#define ESP32_CALDAV_CLIENT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief CalDAV error codes.
 */
typedef enum {
    CALDAV_ERROR_OK = 0,            /**< Operation successful. */
    CALDAV_ERROR_INVALID_ARG,       /**< Invalid argument provided. */
    CALDAV_ERROR_NO_MEM,            /**< Out of memory. */
    CALDAV_ERROR_FAIL,              /**< General failure. */
    CALDAV_ERROR_NOT_INITIALIZED,   /**< CalDAV client not initialized. */
    CALDAV_ERROR_CONNECTION,        /**< Connection error. */
    CALDAV_ERROR_HTTP,              /**< HTTP protocol error. */
    CALDAV_ERROR_TIMEOUT,           /**< Operation timeout. */
} CalDAV_Error_t;

/** @brief CalDAV client configuration.
 */
typedef struct {
    const char *ServerURL;          /**< CalDAV server URL (e.g. https://cloud.example.com/remote.php/dav). */
    const char *Username;           /**< Username for authentication. */
    const char *Password;           /**< Password for authentication. */
    const char *CalendarPath;      /**< Path to calendar (e.g. /calendars/user/calendar-name/). */
    uint32_t TimeoutMs;            /**< Timeout in milliseconds. */
    bool UseHTTPS;                 /**< true for HTTPS, false for HTTP. */
} CalDAV_Config_t;

/** @brief CalDAV client handle. */
typedef struct CalDAV_Client_t CalDAV_Client_t;

/** @brief Calendar information structure.
 */
typedef struct {
    char *Name;                     /**< Calendar name (extracted from path). */
    char *Path;                     /**< Calendar path (relative to server). */
    char *DisplayName;             /**< Display name for UI. */
    char *Description;              /**< Calendar description (optional). */
    char *Color;                    /**< Calendar color in hex format (optional). */
} CalDAV_Calendar_t;

/** @brief Calendar event data structure.
 */
typedef struct {
    char *uid;                      /**< Event unique identifier. */
    char *summary;                  /**< Event title/summary. */
    char *description;              /**< Event description (optional). */
    char *start_time;               /**< Start time in ISO 8601 format. */
    char *end_time;                 /**< End time in ISO 8601 format. */
    char *location;                 /**< Event location (optional). */
} CalDAV_Calendar_Event_t;

/** @brief          Initializes the CalDAV client with given configuration.
 *  @param p_Config Pointer to configuration structure (must not be NULL)
 *  @return         CalDAV client handle on success, NULL on failure
 */
CalDAV_Client_t *CalDAV_Client_Init(const CalDAV_Config_t *p_Config);

/** @brief          Deinitializes and frees the CalDAV client.
 *  @param p_Client CalDAV client handle
 */
void CalDAV_Client_Deinit(CalDAV_Client_t *p_Client);

/** @brief          Tests the connection to the CalDAV server.
 *  @param p_Client CalDAV client handle (must not be NULL)
 *  @return         CALDAV_ERROR_OK on success, error code otherwise
 */
CalDAV_Error_t CalDAV_Test_Connection(CalDAV_Client_t *p_Client);

/** @brief              Lists all available calendars from the CalDAV server.
 *  @param p_Client     CalDAV client handle (must not be NULL)
 *  @param p_Calendars  Pointer to calendar array pointer (will be allocated, caller must free with CalDAV_Calendars_Free)
 *  @param p_Length     Pointer to store the number of calendars found
 *  @return             CALDAV_ERROR_OK on success, error code otherwise
 */
CalDAV_Error_t CalDAV_Calendars_List(CalDAV_Client_t *p_Client,
                                      CalDAV_Calendar_t **p_Calendars,
                                      size_t *p_Length);

/** @brief              Lists all events from the configured calendar.
 *  @param p_Client     CalDAV client handle (must not be NULL, calendar_path must be set in config)
 *  @param p_Events     Pointer to event array pointer (will be allocated, caller must free with CalDAV_Events_Free)
 *  @param p_Length     Pointer to store the number of events found
 *  @param p_StartTime  Time range filter start in iCalendar format (YYYYMMDDTHHMMSSZ, e.g. "20200101T000000Z")
 *  @param p_EndTime    Time range filter end in iCalendar format (YYYYMMDDTHHMMSSZ, e.g. "20301231T235959Z")
 *  @return             CALDAV_ERROR_OK on success, error code otherwise
 */
CalDAV_Error_t CalDAV_Calendar_Events_List(CalDAV_Client_t *p_Client,
                                            CalDAV_Calendar_Event_t **p_Events,
                                            size_t *p_Length,
                                            const char *p_StartTime,
                                            const char *p_EndTime);

/** @brief          Frees memory allocated for event data.
 *  @param p_Events Event array to free
 *  @param Length   Number of events in the array
 */
void CalDAV_Events_Free(CalDAV_Calendar_Event_t *p_Events, size_t Length);

/** @brief              Frees memory allocated for calendar data.
 *  @param p_Calendars  Calendar array to free
 *  @param Length       Number of calendars in the array
 */
void CalDAV_Calendars_Free(CalDAV_Calendar_t *p_Calendars, size_t Length);

/** @brief              Retrieves a specific calendar event.
 *  @param p_Client     CalDAV client handle (must not be NULL)
 *  @param p_EventPath  Path to the event resource (e.g. "event.ics")
 *  @param p_Event      Pointer to event structure to fill
 *  @return             CALDAV_ERROR_OK on success, error code otherwise
 *  @note               This function is not yet fully implemented
 */
CalDAV_Error_t CalDAV_Calendar_Event_Get(CalDAV_Client_t *p_Client,
                                          const char *p_EventPath,
                                          CalDAV_Calendar_Event_t *p_Event);

#ifdef __cplusplus
}
#endif

#endif // ESP32_CALDAV_CLIENT_H_
