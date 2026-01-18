/*
 * caldav_client.c
 *
 *  Copyright (C) Daniel Kampert, 2026
 *  Website: www.kampis-elektroecke.de
 *  File info: CalDAV client implementation with CalDAV support for calendar and event management.
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

#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "caldav_client.h"

static const char *TAG = "CalDAV";

#define CALDAV_HTTP_BUFFER_LENGTH    4096

/** @brief HTTP client configuration for CalDAV.
 */
static esp_http_client_config_t _CalDav_HTTP_Config = {
        .url = NULL,
        .method = HTTP_METHOD_PROPFIND,
        .timeout_ms = 0,
        .event_handler = NULL,
        .user_data = NULL,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,
        .use_global_ca_store = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .cert_pem = NULL,
        .client_cert_pem = NULL,
        .client_key_pem = NULL,
        .if_name = NULL,
};

/** @brief
 */
struct CalDAV_Client_t {
    CalDAV_Config_t config;
    bool is_initialized;
};

/** @brief
*/
typedef struct {
    char *buffer;
    size_t size;
    size_t position;
} http_response_buffer_t;

/** @brief          HTTP Event Handler
 *  @param p_Event  Pointer to HTTP Event
 *  @return         ESP_OK on success
 */
static esp_err_t on_HTTP_Event_Handler(esp_http_client_event_t *p_Event)
{
    http_response_buffer_t *p_OutputBuffer = (http_response_buffer_t*)p_Event->user_data;

    switch(p_Event->event_id) {
        case HTTP_EVENT_ON_DATA: {
            /* Allocate buffer if not yet available */
            if (p_OutputBuffer->buffer == NULL) {
                p_OutputBuffer->buffer = malloc(CALDAV_HTTP_BUFFER_LENGTH);
                p_OutputBuffer->size = CALDAV_HTTP_BUFFER_LENGTH;
                p_OutputBuffer->position = 0;

                if (p_OutputBuffer->buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate response buffer");
                    return ESP_FAIL;
                }
            }

            /* Expand buffer if necessary */
            if ((p_OutputBuffer->position + p_Event->data_len) >= p_OutputBuffer->size) {
                size_t NewSize;
                char *p_NewBuffer;

                NewSize = p_OutputBuffer->size * 2;
                while ((p_OutputBuffer->position + p_Event->data_len) >= NewSize) {
                    NewSize *= 2;
                }

                p_NewBuffer = realloc(p_OutputBuffer->buffer, NewSize);
                if (p_NewBuffer == NULL) {
                    ESP_LOGE(TAG, "Failed to expand response buffer");

                    return ESP_FAIL;
                }

                p_OutputBuffer->buffer = p_NewBuffer;
                p_OutputBuffer->size = NewSize;
                ESP_LOGI(TAG, "Response buffer expanded to %d bytes", NewSize);
            }

            /* Copy data into buffer */
            memcpy(p_OutputBuffer->buffer + p_OutputBuffer->position, p_Event->data, p_Event->data_len);
            p_OutputBuffer->position += p_Event->data_len;
            p_OutputBuffer->buffer[p_OutputBuffer->position] = 0;

            break;
        }
        default: {
            break;
        }
    }

    return ESP_OK;
}

/** @brief          Extracts a value from an iCal field.
 *  @param p_Data   Pointer to iCal data
 *  @param p_Field  Field name (e.g. "SUMMARY:")
 *  @return         Duplicated string with the value or NULL
 */
static char* extract_ical_field(const char *p_Data, const char *p_Field)
{
    char *Start;
    char *End;
    char *Value;
    
    Start = strstr(p_Data, p_Field);
    if (Start == NULL) {
        return NULL;
    }

    Start += strlen(p_Field);
    End = strchr(Start, '\n');
    if (End == NULL) {
        End = strchr(Start, '\r');
    }

    if (End == NULL) {
        return strdup(Start);
    }

    Value = malloc(End - Start + 1);
    if (Value) {
        memcpy(Value, Start, End - Start);
        Value[End - Start] = '\0';

        /* Remove possible \r at the end */
        if (((End - Start) > 0) && (Value[End - Start - 1] == '\r')) {
            Value[End - Start - 1] = '\0';
        }
    }

    return Value;
}

/** @brief          Extracts a value from XML between tags.
 *  @param p_Data   Pointer to XML data
 *  @param p_Tag    Tag name (e.g. "displayname")
 *  @return         Duplicated string with the value or NULL
 */
static char* extract_xml_tag_value(const char *p_Data, const char *p_Tag)
{
    char StartTag[128];
    char EndTag[128];
    char *Start = NULL;
    char *End;
    char *Value;

    /* Try first: <tag> */
    snprintf(StartTag, sizeof(StartTag), "<%s>", p_Tag);
    Start = strstr(p_Data, StartTag);

    if (Start == NULL) {
        /* Try with namespace prefix: :tag> */
        snprintf(StartTag, sizeof(StartTag), ":%s>", p_Tag);
        Start = strstr(p_Data, StartTag);
    }

    if (Start == NULL) {
        return NULL;
    }

    /* Find the end of the opening tag (the '>') */
    Start = strchr(Start, '>');
    if (Start == NULL) {
        return NULL;
    }

    /* Move to the start of the content (after the '>') */
    Start++;

    /* Search for closing tag */
    snprintf(EndTag, sizeof(EndTag), "</%s>", p_Tag);
    End = strstr(Start, EndTag);
    if (End == NULL) {
        /* Try also with namespace prefix */
        snprintf(EndTag, sizeof(EndTag), ":/%s>", p_Tag);
        End = strstr(Start, EndTag);

        if (End != NULL) {
            /* Go back to '<' */
            while ((End > Start) && (*(End - 1) != '<')) {
                End--;
            }

            End--;
        }
    }
    
    if (End == NULL) {
        return NULL;
    }

    Value = malloc(End - Start + 1);
    if (Value) {
        memcpy(Value, Start, End - Start);
        Value[End - Start] = '\0';
    }

    return Value;
}

CalDAV_Client_t* CalDAV_Client_Init(const CalDAV_Config_t *p_Config)
{
    CalDAV_Client_t *Client;

    if ((p_Config == NULL) || (p_Config->server_url == NULL) || (p_Config->username == NULL) || (p_Config->password == NULL)) {
        ESP_LOGE(TAG, "Invalid configuration!");

        return NULL;
    }

    Client = calloc(1, sizeof(CalDAV_Client_t));
    if (Client == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed for CalDAV client!");

        return NULL;
    }

    Client->config = *p_Config;
    Client->config.server_url = strdup(p_Config->server_url);
    Client->config.username = strdup(p_Config->username);
    Client->config.password = strdup(p_Config->password);

    if (p_Config->calendar_path) {
        Client->config.calendar_path = strdup(p_Config->calendar_path);
    }

    Client->is_initialized = true;

    ESP_LOGD(TAG, "CalDAV client initialized: %s", p_Config->server_url);

    return Client;
}

void CalDAV_Client_Deinit(CalDAV_Client_t *p_Client)
{
    if (p_Client == NULL) {
        return;
    }

    if (p_Client->config.server_url) {
        free((void*)p_Client->config.server_url);
    }

    if (p_Client->config.username) {
        free((void*)p_Client->config.username);
    }

    if (p_Client->config.password) {
        free((void*)p_Client->config.password);
    }

    if (p_Client->config.calendar_path) {
        free((void*)p_Client->config.calendar_path);
    }

    free(p_Client);
}

CalDAV_Error_t CalDAV_Test_Connection(CalDAV_Client_t *p_Client)
{
    esp_err_t Error;
    int StatusCode;
    http_response_buffer_t Response = {0};
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->is_initialized == false)) {
        return CALDAV_ERR_NOT_INITIALIZED;
    }

    _CalDav_HTTP_Config.timeout_ms = p_Client->config.timeout_ms;
    _CalDav_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDav_HTTP_Config.url = p_Client->config.server_url;
    _CalDav_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDav_HTTP_Config);
    if (HTTP_Client == NULL) {
        ESP_LOGE(TAG, "HTTP client initialization failed!");

        return CALDAV_ERR_FAIL;
    }

    esp_http_client_set_username(HTTP_Client, p_Client->config.username);
    esp_http_client_set_password(HTTP_Client, p_Client->config.password);
    esp_http_client_set_authtype(HTTP_Client, HTTP_AUTH_TYPE_BASIC);

    esp_http_client_set_header(HTTP_Client, "Depth", "0");

    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);

    esp_http_client_cleanup(HTTP_Client);
    if (Response.buffer) {
        free(Response.buffer);
    }

    if (Error != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Request failed: %d!", Error);
        return CALDAV_ERR_CONNECTION;
    }

    if ((StatusCode == 200) || (StatusCode == 204) || (StatusCode == 207)) {
        ESP_LOGI(TAG, "CalDAV connection successful (Status: %d)", StatusCode);

        return CALDAV_ERR_OK;
    } else if (StatusCode == 401) {
        ESP_LOGE(TAG, "Authentication failed (Status: 401)!");

        return CALDAV_ERR_HTTP;
    } else {
        ESP_LOGW(TAG, "Unexpected status code: %d!", StatusCode);

        return CALDAV_ERR_HTTP;
    }
}

CalDAV_Error_t CalDAV_Calendars_List(CalDAV_Client_t *p_Client,
                                     CalDAV_Calendar_t **p_Calendars,
                                     size_t *p_Length)
{
    char url[512];
    esp_err_t Error;
    int StatusCode;
    http_response_buffer_t Response = {0};
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->is_initialized == false) || (p_Calendars == NULL) || (p_Length == NULL)) {
        return CALDAV_ERR_INVALID_ARG;
    }

    /* PROPFIND request to find all calendars */
    const char *propfind_body = 
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
        "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\" xmlns:CS=\"http://calendarserver.org/ns/\">\n"
        "  <D:prop>\n"
        "    <D:resourcetype/>\n"
        "    <D:displayname/>\n"
        "    <C:calendar-description/>\n"
        "    <CS:getctag/>\n"
        "  </D:prop>\n"
        "</D:propfind>";

    /* Use server URL as base for calendar search */
    snprintf(url, sizeof(url), "%s", p_Client->config.server_url);

    ESP_LOGI(TAG, "Searching calendars on: %s", url);

    _CalDav_HTTP_Config.timeout_ms = p_Client->config.timeout_ms;
    _CalDav_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDav_HTTP_Config.url = url;
    _CalDav_HTTP_Config.method = HTTP_METHOD_PROPFIND;
    _CalDav_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDav_HTTP_Config);
    if (HTTP_Client == NULL) {
        return CALDAV_ERR_FAIL;
    }

    esp_http_client_set_username(HTTP_Client, p_Client->config.username);
    esp_http_client_set_password(HTTP_Client, p_Client->config.password);
    esp_http_client_set_authtype(HTTP_Client, HTTP_AUTH_TYPE_BASIC);

    esp_http_client_set_header(HTTP_Client, "Content-Type", "application/xml; charset=utf-8");
    esp_http_client_set_header(HTTP_Client, "Depth", "1");
    esp_http_client_set_method(HTTP_Client, HTTP_METHOD_PROPFIND);

    esp_http_client_set_post_field(HTTP_Client, propfind_body, strlen(propfind_body));

    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);

    esp_http_client_cleanup(HTTP_Client);

    if (Error != ESP_OK) {
        ESP_LOGE(TAG, "Calendar PROPFIND failed: %d", Error);
        if (Response.buffer) {
            free(Response.buffer);
        }

        return CALDAV_ERR_HTTP;
    }

    if ((StatusCode != 200) && (StatusCode != 207)) {
        ESP_LOGE(TAG, "Calendar PROPFIND unexpected status: %d", StatusCode);
        if (Response.buffer) {
            free(Response.buffer);
        }

        return CALDAV_ERR_HTTP;
    }

    ESP_LOGI(TAG, "Calendar response received");
    ESP_LOGI(TAG, "Response buffer: %s, position: %d", Response.buffer ? "available" : "NULL", Response.position);

    if (Response.buffer) {
        int CurrentCalendar = 0;
        int CalendarCount = 0;
        char *Pos = Response.buffer;
        char *TempPos = Pos;

#if DEBUG
        /* Debug: Show response size and content */
        char Preview[1001];
        size_t PreviewLen = Response.position < 1000 ? Response.position : 1000;
        memcpy(Preview, Response.buffer, PreviewLen);
        Preview[PreviewLen] = '\0';

        ESP_LOGD(TAG, "Response size: %d bytes", Response.position);
        ESP_LOGD(TAG, "Response (first 1000 characters):");
        ESP_LOGD(TAG, "%s", Preview);
#endif

        if ((strstr(Response.buffer, "<!DOCTYPE html>") != NULL) || 
            (strstr(Response.buffer, "<html>") != NULL) ||
            (strstr(Response.buffer, "<html ") != NULL)) {
            ESP_LOGE(TAG, "Invalid XML!");

            *p_Length = 0;
            *p_Calendars = NULL;

            free(Response.buffer);

            return CALDAV_ERR_HTTP;
        }

        /* Search for :calendar (with arbitrary namespace prefix) */
        while ((TempPos = strstr(TempPos, ":calendar")) != NULL) {
            /* Check if there is a '<' and a letter before (e.g. <C:calendar, <C1:calendar) */
            if (TempPos > Response.buffer) {
                char *TagStart = TempPos - 1;

                /* Go back to the '<' */
                while ((TagStart > Response.buffer) && (*TagStart != '<') && (*TagStart != ' ')) {
                    TagStart--;
                }
                if (*TagStart == '<') {
                    CalendarCount++;
                    ESP_LOGD(TAG, "Found calendar tag");
                }
            }

            TempPos += 9;
        }

        ESP_LOGD(TAG, "Potential calendars found: %d", CalendarCount);

        if (CalendarCount == 0) {
            *p_Length = 0;
            *p_Calendars = NULL;

            free(Response.buffer);

            return CALDAV_ERR_OK;
        }

        /* Allocate memory for all calendars */
        *p_Calendars = calloc(CalendarCount, sizeof(caldav_calendar_t));
        if (*p_Calendars == NULL) {
            free(Response.buffer);

            return CALDAV_ERR_NO_MEM;
        }
        
        /* Parse each calendar (search for <D:response> blocks with calendar resourcetype) */
        Pos = Response.buffer;

        while ((Pos = strstr(Pos, "<response>")) != NULL && (CurrentCalendar < CalendarCount)) {
            char *ResponseEnd = strstr(Pos, "</response>");
            char *CalendarType;

            if (ResponseEnd == NULL) {
                break;
            }

            /* Check if this response contains a calendar (search for :calendar tag) */
            CalendarType = strstr(Pos, ":calendar");

            if ((CalendarType != NULL) && (CalendarType < ResponseEnd)) {
                /* Extract calendar data */
                size_t ResponseLen = ResponseEnd - Pos;
                char *ResponseData = malloc(ResponseLen + 1);

                if (ResponseData) {
                    char *Href;

                    memcpy(ResponseData, Pos, ResponseLen);
                    ResponseData[ResponseLen] = '\0';

                    ESP_LOGI(TAG, "Processing calendar response block");

                    /* Extract href (path) */
                    Href = extract_xml_tag_value(ResponseData, "href");
                    if (Href) {
                        char *LastSlash;

                        (*p_Calendars)[CurrentCalendar].path = Href;

                        ESP_LOGI(TAG, "  href: %s", Href);

                        /* Extract calendar name from path */
                        LastSlash = strrchr(Href, '/');
                        if (LastSlash && *(LastSlash + 1) != '\0') {
                            (*p_Calendars)[CurrentCalendar].name = strdup(LastSlash + 1);
                        } else if (LastSlash && (LastSlash > Href)) {
                            /* Path ends with /, take the previous part */
                            char *PrevSlash = LastSlash - 1;
                            while ((PrevSlash > Href) && (*PrevSlash != '/')) {
                                PrevSlash--;
                            }

                            if (*PrevSlash == '/') {
                                (*p_Calendars)[CurrentCalendar].name = malloc(LastSlash - PrevSlash);
                                if ((*p_Calendars)[CurrentCalendar].name) {
                                    memcpy((*p_Calendars)[CurrentCalendar].name, PrevSlash + 1, LastSlash - PrevSlash - 1);
                                    (*p_Calendars)[CurrentCalendar].name[LastSlash - PrevSlash - 1] = '\0';
                                }
                            }
                        }
                    }

                    (*p_Calendars)[CurrentCalendar].display_name = extract_xml_tag_value(ResponseData, "displayname");
                    (*p_Calendars)[CurrentCalendar].description = extract_xml_tag_value(ResponseData, "calendar-description");

                    ESP_LOGI(TAG, "Calendar %d:", CurrentCalendar + 1);
                    if ((*p_Calendars)[CurrentCalendar].name) {
                        ESP_LOGI(TAG, "  Name: %s", (*p_Calendars)[CurrentCalendar].name);
                    }

                    if ((*p_Calendars)[CurrentCalendar].display_name) {
                        ESP_LOGI(TAG, "  Display name: %s", (*p_Calendars)[CurrentCalendar].display_name);
                    }

                    if ((*p_Calendars)[CurrentCalendar].path) {
                        ESP_LOGI(TAG, "  Path: %s", (*p_Calendars)[CurrentCalendar].path);
                    }

                    free(ResponseData);
                    CurrentCalendar++;
                }
            }

            Pos = ResponseEnd + sizeof("</response>") - 1;
        }

        *p_Length = CurrentCalendar;
        free(Response.buffer);
    } else {
        *p_Length = 0;
        *p_Calendars = NULL;
    }

    return CALDAV_ERR_OK;
}

void CalDAV_Calendars_Free(caldav_calendar_t *p_Calendars, size_t Length)
{
    if (p_Calendars == NULL) {
        return;
    }

    for (size_t i = 0; i < Length; i++) {
        if (p_Calendars[i].name) free(p_Calendars[i].name);
        if (p_Calendars[i].path) free(p_Calendars[i].path);
        if (p_Calendars[i].display_name) free(p_Calendars[i].display_name);
        if (p_Calendars[i].description) free(p_Calendars[i].description);
        if (p_Calendars[i].color) free(p_Calendars[i].color);
    }

    free(p_Calendars);
}

CalDAV_Error_t CalDAV_Calendar_Events_List(CalDAV_Client_t *p_Client,
                                           caldav_calendar_event_t **events,
                                           size_t *Length,
                                           const char *p_StartTime,
                                           const char *p_EndTime)
{
    char url[512];
    esp_err_t Error;
    int StatusCode;
    http_response_buffer_t Response = {0};
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->is_initialized == false) || (events == NULL) || (Length == NULL) || (p_StartTime == NULL) || (p_EndTime == NULL)) {
        return CALDAV_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Time range: %s to %s", p_StartTime, p_EndTime);

    char propfind_body[1024];
    snprintf(propfind_body, sizeof(propfind_body),
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
        "<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
        "  <D:prop>\n"
        "    <D:getetag/>\n"
        "    <C:calendar-data/>\n"
        "  </D:prop>\n"
        "  <C:filter>\n"
        "    <C:comp-filter name=\"VCALENDAR\">\n"
        "      <C:comp-filter name=\"VEVENT\">\n"
        "        <C:time-range start=\"%s\" end=\"%s\"/>\n"
        "      </C:comp-filter>\n"
        "    </C:comp-filter>\n"
        "  </C:filter>\n"
        "</C:calendar-query>", p_StartTime, p_EndTime);

    snprintf(url, sizeof(url), "%s%s", 
             p_Client->config.server_url,
             p_Client->config.calendar_path ? p_Client->config.calendar_path : "/");

    ESP_LOGI(TAG, "CalDAV Query URL: %s", url);
    ESP_LOGI(TAG, "calendar_path: '%s'", p_Client->config.calendar_path ? p_Client->config.calendar_path : "(empty)");

    _CalDav_HTTP_Config.timeout_ms = p_Client->config.timeout_ms;
    _CalDav_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDav_HTTP_Config.url = url;
    _CalDav_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDav_HTTP_Config);
    if (HTTP_Client == NULL) {
        return CALDAV_ERR_FAIL;
    }

    esp_http_client_set_username(HTTP_Client, p_Client->config.username);
    esp_http_client_set_password(HTTP_Client, p_Client->config.password);
    esp_http_client_set_authtype(HTTP_Client, HTTP_AUTH_TYPE_BASIC);

    /* CalDAV calendar-query requires REPORT method, not PROPFIND */
    esp_http_client_set_method(HTTP_Client, HTTP_METHOD_POST);
    esp_http_client_set_header(HTTP_Client, "Content-Type", "application/xml; charset=utf-8");
    esp_http_client_set_header(HTTP_Client, "Depth", "1");

    /* Override method to use custom REPORT method */
    esp_http_client_set_header(HTTP_Client, "X-HTTP-Method-Override", "REPORT");

    esp_http_client_set_post_field(HTTP_Client, propfind_body, strlen(propfind_body));

    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);

    esp_http_client_cleanup(HTTP_Client);

    if ((Error != ESP_OK) || (StatusCode != 207)) {
        ESP_LOGE(TAG, "CalDAV-Query failed: %d (Status: %d)!", Error, StatusCode);
        if (Response.buffer) {
            free(Response.buffer);
        }

        return CALDAV_ERR_HTTP;
    }

    ESP_LOGI(TAG, "CalDAV response received");
    if (Response.buffer) {
        int EventCount = 0;
        int CurrentEvent = 0;
        char *p_Pos = Response.buffer;

#if DEBUG
        /* Debug: Show first part of the response */
        char Preview[501];
        size_t PreviewLen = Response.position < 500 ? Response.position : 500;
        memcpy(Preview, Response.buffer, PreviewLen);
        Preview[PreviewLen] = '\0';

        ESP_LOGI(TAG, "Response (first 500 characters):");
        ESP_LOGI(TAG, "%s", Preview);
#endif

        /* Count events in the response */
        while ((p_Pos = strstr(p_Pos, "BEGIN:VEVENT")) != NULL) {
            EventCount++;
            p_Pos += 12;
        }

        ESP_LOGD(TAG, "Found: %d events", EventCount);

        if (EventCount == 0) {
            *Length = 0;
            *events = NULL;

            free(Response.buffer);

            return CALDAV_ERR_OK;
        }

        /* Allocate memory for all events */
        *events = calloc(EventCount, sizeof(caldav_calendar_event_t));
        if (*events == NULL) {
            free(Response.buffer);

            return CALDAV_ERR_NO_MEM;
        }

        /* Parse each event */
        p_Pos = Response.buffer;
        
        while ((p_Pos = strstr(p_Pos, "BEGIN:VEVENT")) != NULL && CurrentEvent < EventCount) {
            size_t EventLen;
            char *EventEnd;
            char *EventData;

            EventEnd = strstr(p_Pos, "END:VEVENT");

            if (EventEnd == NULL) {
                break;
            }

            /* Extract event data */
            EventLen = EventEnd - p_Pos + 10;
            EventData = malloc(EventLen + 1);
            if (EventData) {
                memcpy(EventData, p_Pos, EventLen);
                EventData[EventLen] = '\0';

                /* Parse iCal fields */
                (*events)[CurrentEvent].summary = extract_ical_field(EventData, "SUMMARY:");
                (*events)[CurrentEvent].description = extract_ical_field(EventData, "DESCRIPTION:");
                (*events)[CurrentEvent].location = extract_ical_field(EventData, "LOCATION:");
                (*events)[CurrentEvent].uid = extract_ical_field(EventData, "UID:");
                (*events)[CurrentEvent].start_time = extract_ical_field(EventData, "DTSTART");
                (*events)[CurrentEvent].end_time = extract_ical_field(EventData, "DTEND");

                /* Log event details */
                if ((*events)[CurrentEvent].summary) {
                    ESP_LOGI(TAG, "Event %d: %s", CurrentEvent + 1, (*events)[CurrentEvent].summary);
                }

                free(EventData);
                CurrentEvent++;
            }

            p_Pos = EventEnd + 10;
        }

        *Length = CurrentEvent;
        free(Response.buffer);
    } else {
        *Length = 0;
        *events = NULL;
    }

    return CALDAV_ERR_OK;
}

CalDAV_Error_t CalDAV_Calendar_Event_Get(CalDAV_Client_t *p_Client,
                                         const char *EventPath,
                                         caldav_calendar_event_t *p_Event)
{
    char url[512];
    esp_err_t Error;
    int StatusCode;
    http_response_buffer_t Response = {0};
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->is_initialized == false) || (EventPath == NULL) || (p_Event == NULL)) {
        return CALDAV_ERR_INVALID_ARG;
    }

    snprintf(url, sizeof(url), "%s%s%s",
             p_Client->config.server_url,
             p_Client->config.calendar_path ? p_Client->config.calendar_path : "/",
             EventPath);

    _CalDav_HTTP_Config.timeout_ms = p_Client->config.timeout_ms;
    _CalDav_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDav_HTTP_Config.url = url;
    _CalDav_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDav_HTTP_Config);
    if (HTTP_Client == NULL) {
        return CALDAV_ERR_FAIL;
    }

    esp_http_client_set_username(HTTP_Client, p_Client->config.username);
    esp_http_client_set_password(HTTP_Client, p_Client->config.password);
    esp_http_client_set_authtype(HTTP_Client, HTTP_AUTH_TYPE_BASIC);

    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);

    esp_http_client_cleanup(HTTP_Client);

    if ((Error != ESP_OK) || (StatusCode != 200)) {
        ESP_LOGE(TAG, "Event download failed: %d (Status: %d)!", Error, StatusCode);
        if (Response.buffer) {
            free(Response.buffer);
        }

        return CALDAV_ERR_HTTP;
    }

    if (Response.buffer) {
        ESP_LOGI(TAG, "Event data received:");
        ESP_LOGI(TAG, "%s", Response.buffer);
        /* TODO: Parse iCal format */
        free(Response.buffer);
    }

    ESP_LOGW(TAG, "iCal parsing not yet implemented");

    return CALDAV_ERR_OK;
}

void CalDAV_Events_Free(caldav_calendar_event_t *p_Events, size_t Length)
{
    if (p_Events == NULL) {
        return;
    }

    for (size_t i = 0; i < Length; i++) {
        if (p_Events[i].uid) {
            free(p_Events[i].uid);
        }

        if (p_Events[i].summary) {
            free(p_Events[i].summary);
        }

        if (p_Events[i].description) {
            free(p_Events[i].description);
        }

        if (p_Events[i].start_time) {
            free(p_Events[i].start_time);
        }

        if (p_Events[i].end_time) {
            free(p_Events[i].end_time);
        }

        if (p_Events[i].location) {
            free(p_Events[i].location);
        }
    }

    free(p_Events);
}
