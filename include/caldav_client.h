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
#include <string>

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
    CALDAV_ERROR_NOT_FOUND,         /**< Resource not found. */
} CalDAV_Error_t;

/** @brief CalDAV client configuration.
 */
typedef struct {
    char ServerURL[256];            /**< CalDAV server URL (e.g. https://cloud.example.com/remote.php/dav). */
    char Username[64];              /**< Username for authentication. */
    char Password[64];              /**< Password for authentication. */
    uint32_t TimeoutMs;             /**< Timeout in milliseconds. */
} CalDAV_Config_t;

/** @brief CalDAV client handle.
 */
typedef struct {
    std::string ServerURL;          /**< CalDAV server URL. */
    std::string Username;           /**< Username for authentication. */
    std::string Password;           /**< Password for authentication. */
    uint32_t TimeoutMs;             /**< Timeout in milliseconds. */
    bool IsInitialized;             /**< Indicates if the client is initialized. */
} CalDAV_Client_t;

/** @brief Calendar information structure.
 */
typedef struct {
    char *Name;                     /**< Calendar name (extracted from path). */
    char *Path;                     /**< Calendar path (relative to server). */
    char *DisplayName;              /**< Display name for UI. */
    char *Description;              /**< Calendar description (optional). */
    char *Color;                    /**< Calendar color in hex format (optional). */
} CalDAV_Calendar_t;

/** @brief Calendar list data structure.
 */
typedef struct {
    CalDAV_Calendar_t *Calendar;    /**< Pointer to an array of calendars. */
    size_t Length;                  /**< Number of calendars in the array. */
} CalDAV_Calendar_List_t;

/** @brief Calendar event data structure.
 */
typedef struct {
    char *UID;                      /**< Event unique identifier. */
    char *Summary;                  /**< Event title/summary. */
    char *Description;              /**< Event description (optional). */
    char *StartTime;                /**< Start time in ISO 8601 format. */
    char *EndTime;                  /**< End time in ISO 8601 format. */
    char *Location;                 /**< Event location (optional). */
} CalDAV_Calendar_Event_t;

#ifdef __cplusplus
extern "C" {
#endif

/** @brief          Initializes the CalDAV client with given configuration.
 *  @param p_Config Pointer to configuration structure (must not be NULL)
 *  @param p_Client Pointer to CalDAV client handle to initialize
 *  @return         CALDAV_ERROR_OK on success, error code otherwise
 */
CalDAV_Error_t CalDAV_Client_Init(const CalDAV_Config_t *p_Config, CalDAV_Client_t *p_Client);

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
 *  @param p_Calendars  Pointer to calendar list (will be allocated, caller must free with CalDAV_Calendars_Free)
 *  @return             CALDAV_ERROR_OK on success, error code otherwise
 */
CalDAV_Error_t CalDAV_Calendars_List(CalDAV_Client_t *p_Client,
                                     CalDAV_Calendar_List_t *p_Calendars);

/** @brief              Finds a calendar by name or display name in the calendar list.
 *  @param p_Calendars  Pointer to calendar list (must not be NULL)
 *  @param p_Name       Calendar name to search for (searches both Name and DisplayName fields)
 *  @param pp_Calendar  Pointer to store the found calendar pointer (NULL if not found)
 *  @return             CALDAV_ERROR_OK if found, CALDAV_ERROR_NOT_FOUND if calendar doesn't exist,
 *                      CALDAV_ERROR_INVALID_ARG if parameters are NULL
 */
CalDAV_Error_t CalDAV_Calendar_Find_By_Name(const CalDAV_Calendar_List_t *p_Calendars,
                                            const char *p_Name,
                                            CalDAV_Calendar_t **pp_Calendar);

/** @brief                  Lists all events from the configured calendar.
 *  @param p_Client         CalDAV client handle (must not be NULL, calendar_path must be set in config)
 *  @param p_Events         Pointer to event array pointer (will be allocated, caller must free with CalDAV_Events_Free)
 *  @param p_Length         Pointer to store the number of events found
 *  @param p_CalendarPath   Path to the calendar resource (e.g. "/calendars/user/calendar-name/")
 *  @param p_StartTime      Pointer to time range filter start as UTC time
 *  @param p_EndTime        Pointer to time range filter end as UTC time
 *  @return                 CALDAV_ERROR_OK on success, error code otherwise
 */
CalDAV_Error_t CalDAV_Calendar_Events_List(CalDAV_Client_t *p_Client,
                                           CalDAV_Calendar_Event_t **p_Events,
                                           size_t *p_Length,
                                           const char *p_CalendarPath,
                                           const struct tm* p_StartTime,
                                           const struct tm* p_EndTime);

/** @brief          Frees memory allocated for event data.
 *  @param p_Events Event array to free
 *  @param Length   Number of events in the array
 */
void CalDAV_Events_Free(CalDAV_Calendar_Event_t *p_Events, size_t Length);

/** @brief              Frees memory allocated for calendar data.
 *  @param p_Calendars  Calendar list to free
 */
void CalDAV_Calendars_Free(CalDAV_Calendar_List_t *p_Calendars);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_CALDAV_CLIENT_H_ */
