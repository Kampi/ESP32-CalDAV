/*
 * caldav_client.cpp
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

#include <esp_log.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <string>

#include "caldav_client.h"

#define CALDAV_HTTP_BUFFER_LENGTH    4096

/* PROPFIND request to find all calendars */
static const char *_CalDAV_Propfind_Body =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
    "<D:propfind xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\" xmlns:CS=\"http://calendarserver.org/ns/\">\n"
    "  <D:prop>\n"
    "    <D:resourcetype/>\n"
    "    <D:displayname/>\n"
    "    <C:calendar-description/>\n"
    "    <CS:getctag/>\n"
    "  </D:prop>\n"
    "</D:propfind>";

static const char *TAG = "CalDAV-Client";

/** @brief      Helper function to convert std::string to char*.
 *  @param str  The string to convert
 *  @return     Allocated char* or NULL if string is empty
 */
static inline char *_CalDAV_String_to_cstr(const std::string &str)
{
    return str.empty() ? NULL : strdup(str.c_str());
}

/** @brief Structure holding configuration and state for a CalDAV client,
 *         including server URL, credentials, calendar path, timeout, and
 *         initialization status.
 */
static thread_local esp_http_client_config_t _CalDAV_HTTP_Config;

/** @brief
 */
struct CalDAV_Client_t {
    std::string ServerURL;
    std::string Username;
    std::string Password;
    std::string CalendarPath;
    uint32_t TimeoutMs;
    bool IsInitialized;
};

/** @brief  Buffer used to accumulate HTTP response data.
 *          This structure holds a dynamically allocated character buffer, its current
 *          total size, and the write position used while assembling the complete
 *          HTTP response body in the esp_http_client event callback.
 */
typedef struct {
    char *Buffer;
    size_t Size;
    size_t Position;
} http_response_buffer_t;

/** @brief          HTTP Event Handler
 *  @param p_Event  Pointer to HTTP Event
 *  @return         ESP_OK on success
 */
static esp_err_t on_HTTP_Event_Handler(esp_http_client_event_t *p_Event)
{
    http_response_buffer_t *p_OutputBuffer = (http_response_buffer_t *)p_Event->user_data;

    switch (p_Event->event_id) {
        case HTTP_EVENT_ON_DATA: {
            /* Allocate buffer if not yet available */
            if (p_OutputBuffer->Buffer == NULL) {
                p_OutputBuffer->Buffer = (char *)malloc(CALDAV_HTTP_BUFFER_LENGTH);
                p_OutputBuffer->Size = CALDAV_HTTP_BUFFER_LENGTH;
                p_OutputBuffer->Position = 0;

                if (p_OutputBuffer->Buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate response buffer!");

                    return ESP_FAIL;
                }
            }

            /* Expand buffer if necessary */
            if ((p_OutputBuffer->Position + p_Event->data_len) >= p_OutputBuffer->Size) {
                size_t NewSize = p_OutputBuffer->Size * 2;
                while ((p_OutputBuffer->Position + p_Event->data_len) >= NewSize) {
                    NewSize *= 2;
                }

                char *p_NewBuffer = (char *)realloc(p_OutputBuffer->Buffer, NewSize);
                if (p_NewBuffer == NULL) {
                    ESP_LOGE(TAG, "Failed to expand response buffer!");

                    return ESP_FAIL;
                }

                p_OutputBuffer->Buffer = p_NewBuffer;
                p_OutputBuffer->Size = NewSize;
                ESP_LOGD(TAG, "Response buffer expanded to %d bytes", NewSize);
            }

            /* Copy data into buffer */
            memcpy(p_OutputBuffer->Buffer + p_OutputBuffer->Position, p_Event->data, p_Event->data_len);
            p_OutputBuffer->Position += p_Event->data_len;
            p_OutputBuffer->Buffer[p_OutputBuffer->Position] = 0;

            break;
        }
        default: {
            break;
        }
    }

    return ESP_OK;
}

/** @brief
 *  @param Data
 *  @param Field
 *  @return
 */
static std::string _CalDAV_Extract_iCal_Field(const std::string &Data, const std::string &Field)
{
    size_t Start = Data.find(Field);
    size_t End;
    std::string Value;

    if (Start == std::string::npos) {
        return std::string();
    }

    Start += Field.length();
    End = Data.find_first_of("\n\r", Start);

    if (End == std::string::npos) {
        return Data.substr(Start);
    }

    Value = Data.substr(Start, End - Start);

    /* Remove possible \r at the end */
    if ((Value.empty() == false) && (Value.back() == '\r')) {
        Value.pop_back();
    }

    return Value;
}

/** @brief
 *  @param Data
 *  @param Tag
 *  @return
 */
static std::string _CalDAV_Extract_XML_Tag_Value(const std::string &Data, const std::string &Tag)
{
    size_t End;
    std::string StartTag = "<" + Tag + ">";
    std::string EndTag = "</" + Tag + ">";
    size_t Start = Data.find(StartTag);

    if (Start == std::string::npos) {
        StartTag = ":" + Tag + ">";
        Start = Data.find(StartTag);
    }

    if (Start == std::string::npos) {
        return std::string();
    }

    Start = Data.find('>', Start);
    if (Start == std::string::npos) {
        return std::string();
    }

    Start++;
    End = Data.find(EndTag, Start);
    if (End == std::string::npos) {
        return std::string();
    }

    return Data.substr(Start, End - Start);
}

CalDAV_Client_t *CalDAV_Client_Init(const CalDAV_Config_t *p_Config)
{
    if ((p_Config == NULL) || (p_Config->ServerURL == NULL) || (p_Config->Username == NULL) ||
        (p_Config->Password == NULL)) {
        ESP_LOGE(TAG, "Invalid configuration!");

        return NULL;
    }

    /* Initialize HTTP config completely to avoid garbage values */
    memset(&_CalDAV_HTTP_Config, 0, sizeof(_CalDAV_HTTP_Config));
    
    _CalDAV_HTTP_Config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    _CalDAV_HTTP_Config.crt_bundle_attach = esp_crt_bundle_attach;

    auto Client = new CalDAV_Client_t();
    if (Client == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed for CalDAV client!");

        return NULL;
    }

    Client->ServerURL = std::string(p_Config->ServerURL);
    Client->Username = std::string(p_Config->Username);
    Client->Password = std::string(p_Config->Password);
    Client->TimeoutMs = p_Config->TimeoutMs;

    if (p_Config->CalendarPath) {
        Client->CalendarPath = std::string(p_Config->CalendarPath);
    }

    Client->IsInitialized = true;

    ESP_LOGD(TAG, "CalDAV client initialized: %s", p_Config->ServerURL);

    return Client;
}

void CalDAV_Client_Deinit(CalDAV_Client_t *p_Client)
{
    if (p_Client == NULL) {
        return;
    }

    delete p_Client;
}

CalDAV_Error_t CalDAV_Test_Connection(CalDAV_Client_t *p_Client)
{
    esp_err_t Error;
    int StatusCode;
    http_response_buffer_t Response;
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->IsInitialized == false)) {
        return CALDAV_ERROR_NOT_INITIALIZED;
    }

    memset(&Response, 0, sizeof(Response));

    _CalDAV_HTTP_Config.timeout_ms = p_Client->TimeoutMs;
    _CalDAV_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDAV_HTTP_Config.url = p_Client->ServerURL.c_str();
    _CalDAV_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDAV_HTTP_Config);
    if (HTTP_Client == NULL) {
        ESP_LOGE(TAG, "HTTP client initialization failed!");

        return CALDAV_ERROR_FAIL;
    }

    esp_http_client_set_username(HTTP_Client, p_Client->Username.c_str());
    esp_http_client_set_password(HTTP_Client, p_Client->Password.c_str());
    esp_http_client_set_authtype(HTTP_Client, HTTP_AUTH_TYPE_BASIC);

    esp_http_client_set_header(HTTP_Client, "Depth", "0");

    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);

    esp_http_client_cleanup(HTTP_Client);
    if (Response.Buffer) {
        free(Response.Buffer);
    }

    if (Error != ESP_OK) {
        ESP_LOGE(TAG, "HTTP-Request failed: %d!", Error);

        return CALDAV_ERROR_CONNECTION;
    }

    if ((StatusCode == 200) || (StatusCode == 204) || (StatusCode == 207)) {
        ESP_LOGD(TAG, "CalDAV connection successful (Status: %d)", StatusCode);

        return CALDAV_ERROR_OK;
    } else if (StatusCode == 401) {
        ESP_LOGE(TAG, "Authentication failed (Status: 401)!");

        return CALDAV_ERROR_HTTP;
    } else {
        ESP_LOGW(TAG, "Unexpected status code: %d!", StatusCode);

        return CALDAV_ERROR_HTTP;
    }
}

CalDAV_Error_t CalDAV_Calendars_List(CalDAV_Client_t *p_Client,
                                     CalDAV_Calendar_t **p_Calendars,
                                     size_t *p_Length)
{
    char url[512];
    esp_err_t Error;
    int StatusCode;
    http_response_buffer_t Response;
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->IsInitialized == false) || (p_Calendars == NULL) || (p_Length == NULL)) {
        return CALDAV_ERROR_INVALID_ARG;
    }

    memset(&Response, 0, sizeof(Response));

    /* Use server URL as base for calendar search */
    snprintf(url, sizeof(url), "%s", p_Client->ServerURL.c_str());

    ESP_LOGD(TAG, "Searching calendars on: %s", url);

    _CalDAV_HTTP_Config.timeout_ms = p_Client->TimeoutMs;
    _CalDAV_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDAV_HTTP_Config.url = url;
    _CalDAV_HTTP_Config.method = HTTP_METHOD_PROPFIND;
    _CalDAV_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDAV_HTTP_Config);
    if (HTTP_Client == NULL) {
        return CALDAV_ERROR_FAIL;
    }

    esp_http_client_set_username(HTTP_Client, p_Client->Username.c_str());
    esp_http_client_set_password(HTTP_Client, p_Client->Password.c_str());
    esp_http_client_set_authtype(HTTP_Client, HTTP_AUTH_TYPE_BASIC);

    esp_http_client_set_header(HTTP_Client, "Content-Type", "application/xml; charset=utf-8");
    esp_http_client_set_header(HTTP_Client, "Depth", "1");
    esp_http_client_set_method(HTTP_Client, HTTP_METHOD_PROPFIND);

    esp_http_client_set_post_field(HTTP_Client, _CalDAV_Propfind_Body, strlen(_CalDAV_Propfind_Body));

    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);

    esp_http_client_cleanup(HTTP_Client);

    if (Error != ESP_OK) {
        ESP_LOGE(TAG, "Calendar PROPFIND failed: %d", Error);
        if (Response.Buffer) {
            free(Response.Buffer);
        }

        return CALDAV_ERROR_HTTP;
    }

    if ((StatusCode != 200) && (StatusCode != 207)) {
        ESP_LOGE(TAG, "Calendar PROPFIND unexpected status: %d", StatusCode);
        if (Response.Buffer) {
            free(Response.Buffer);
        }

        return CALDAV_ERROR_HTTP;
    }

    ESP_LOGD(TAG, "Calendar response received");
    ESP_LOGD(TAG, "Response buffer: %s, position: %zu", Response.Buffer ? "available" : "NULL", Response.Position);

    if (Response.Buffer) {
        int CalendarCount = 0;
        int CurrentCalendar = 0;
        size_t SearchPos = 0;

        std::string ResponseStr(Response.Buffer);
        free(Response.Buffer);

        /* Check for HTML response (indicates error) */
        if (ResponseStr.find("<!DOCTYPE html>") != std::string::npos ||
            ResponseStr.find("<html>") != std::string::npos ||
            ResponseStr.find("<html ") != std::string::npos) {
            ESP_LOGE(TAG, "Invalid XML!");

            *p_Length = 0;
            *p_Calendars = NULL;

            return CALDAV_ERROR_HTTP;
        }

        /* Count calendar tags */
        while ((SearchPos = ResponseStr.find(":calendar", SearchPos)) != std::string::npos) {
            bool isCalendarTag = false;
            if (SearchPos > 0) {
                /* Walk backwards over the namespace prefix (letters, digits) and optional '/' for end tags */
                size_t i = SearchPos;
                while (i > 0) {
                    char ch = ResponseStr[i - 1];

                    if (((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z')) ||
                        ((ch >= '0') && (ch <= '9')) || (ch == '/')) {
                        --i;
                    }
                    else {
                        break;
                    }
                }

                /* Valid tag if prefix is preceded by '<', e.g. <C1:calendar> or </C1:calendar> */
                if ((i > 0) && (ResponseStr[i - 1] == '<')) {
                    isCalendarTag = true;
                }
            }

            if (isCalendarTag) {
                CalendarCount++;
                ESP_LOGD(TAG, "Found calendar tag");
            }

            /* Move past this occurrence to continue searching */
            SearchPos += strlen(":calendar");
        }

        ESP_LOGD(TAG, "Potential calendars found: %d", CalendarCount);

        if (CalendarCount == 0) {
            *p_Length = 0;
            *p_Calendars = NULL;
            return CALDAV_ERROR_OK;
        }

        /* Allocate memory for all calendars */
        *p_Calendars = (CalDAV_Calendar_t *)calloc(CalendarCount, sizeof(CalDAV_Calendar_t));
        if (*p_Calendars == NULL) {
            *p_Length = 0;
            *p_Calendars = NULL;
            return CALDAV_ERROR_NO_MEM;
        }

        /* Parse each calendar response block */
        SearchPos = 0;

        while (((SearchPos = ResponseStr.find("<response>", SearchPos)) != std::string::npos) &&
               (CurrentCalendar < CalendarCount)) {
            size_t ResponseEnd = ResponseStr.find("</response>", SearchPos);
            if (ResponseEnd == std::string::npos) {
                break;
            }

            /* Extract this response block */
            std::string ResponseBlock = ResponseStr.substr(SearchPos, ResponseEnd - SearchPos);

            /* Check if this response contains a calendar */
            if (ResponseBlock.find(":calendar") != std::string::npos) {
                ESP_LOGD(TAG, "Processing calendar response block");

                /* Extract href (path) */
                std::string HrefStr = _CalDAV_Extract_XML_Tag_Value(ResponseBlock, "href");
                if (HrefStr.empty() == false) {
                    (*p_Calendars)[CurrentCalendar].Path = _CalDAV_String_to_cstr(HrefStr);
                    ESP_LOGD(TAG, "  href: %s", HrefStr.c_str());

                    /* Extract calendar name from path */
                    size_t LastSlashPos = HrefStr.find_last_of('/');
                    if (LastSlashPos != std::string::npos) {
                        if ((LastSlashPos + 1) < HrefStr.length()) {
                            /* Name after last slash */
                            (*p_Calendars)[CurrentCalendar].Name = strdup(HrefStr.substr(LastSlashPos + 1).c_str());
                        } else if (LastSlashPos > 0) {
                            /* Path ends with /, take previous segment */
                            size_t PrevSlashPos = HrefStr.find_last_of('/', LastSlashPos - 1);
                            if (PrevSlashPos != std::string::npos) {
                                std::string NamePart = HrefStr.substr(PrevSlashPos + 1, LastSlashPos - PrevSlashPos - 1);
                                (*p_Calendars)[CurrentCalendar].Name = strdup(NamePart.c_str());
                            }
                        }
                    }
                }

                (*p_Calendars)[CurrentCalendar].DisplayName = _CalDAV_String_to_cstr(
                                                                  _CalDAV_Extract_XML_Tag_Value(ResponseBlock, "displayname"));
                (*p_Calendars)[CurrentCalendar].Description = _CalDAV_String_to_cstr(
                                                                  _CalDAV_Extract_XML_Tag_Value(ResponseBlock, "calendar-description"));

                ESP_LOGD(TAG, "Calendar %d:", CurrentCalendar + 1);
                if ((*p_Calendars)[CurrentCalendar].Name) {
                    ESP_LOGD(TAG, "  Name: %s", (*p_Calendars)[CurrentCalendar].Name);
                }

                if ((*p_Calendars)[CurrentCalendar].DisplayName) {
                    ESP_LOGD(TAG, "  Display name: %s", (*p_Calendars)[CurrentCalendar].DisplayName);
                }

                if ((*p_Calendars)[CurrentCalendar].Path) {
                    ESP_LOGD(TAG, "  Path: %s", (*p_Calendars)[CurrentCalendar].Path);
                }

                CurrentCalendar++;
            }

            SearchPos = ResponseEnd + 1;
        }

        *p_Length = CurrentCalendar;
    } else {
        *p_Length = 0;
        *p_Calendars = NULL;
    }

    return CALDAV_ERROR_OK;
}

CalDAV_Error_t CalDAV_Calendar_Events_List(CalDAV_Client_t *p_Client,
                                           CalDAV_Calendar_Event_t **p_Events,
                                           size_t *Length,
                                           const char *p_StartTime,
                                           const char *p_EndTime)
{
    char url[512];
    esp_err_t Error;
    int StatusCode;
    http_response_buffer_t Response;
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->IsInitialized == false) || (p_Events == NULL) || (Length == NULL) ||
        (p_StartTime == NULL) || (p_EndTime == NULL)) {
        return CALDAV_ERROR_INVALID_ARG;
    }

    memset(&Response, 0, sizeof(Response));

    ESP_LOGD(TAG, "Time range: %s to %s", p_StartTime, p_EndTime);

    snprintf(url, sizeof(url), "%s%s",
             p_Client->ServerURL.c_str(),
             !p_Client->CalendarPath.empty() ? p_Client->CalendarPath.c_str() : "/");

    ESP_LOGD(TAG, "CalDAV Query URL: %s", url);
    ESP_LOGD(TAG, "calendar_path: '%s'", !p_Client->CalendarPath.empty() ? p_Client->CalendarPath.c_str() : "(empty)");

    _CalDAV_HTTP_Config.timeout_ms = p_Client->TimeoutMs;
    _CalDAV_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDAV_HTTP_Config.url = url;
    _CalDAV_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDAV_HTTP_Config);
    if (HTTP_Client == NULL) {
        return CALDAV_ERROR_FAIL;
    }

    esp_http_client_set_username(HTTP_Client, p_Client->Username.c_str());
    esp_http_client_set_password(HTTP_Client, p_Client->Password.c_str());
    esp_http_client_set_authtype(HTTP_Client, HTTP_AUTH_TYPE_BASIC);

    esp_http_client_set_method(HTTP_Client, HTTP_METHOD_POST);
    esp_http_client_set_header(HTTP_Client, "Content-Type", "application/xml; charset=utf-8");
    esp_http_client_set_header(HTTP_Client, "Depth", "1");
    esp_http_client_set_header(HTTP_Client, "X-HTTP-Method-Override", "REPORT");

    /* Build CalDAV calendar-query with time-range filter */
    std::string RequestBody =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
        "<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">\n"
        "  <D:prop>\n"
        "    <D:getetag/>\n"
        "    <C:calendar-data/>\n"
        "  </D:prop>\n"
        "  <C:filter>\n"
        "    <C:comp-filter name=\"VCALENDAR\">\n"
        "      <C:comp-filter name=\"VEVENT\">\n"
        "        <C:time-range start=\"";
    RequestBody += p_StartTime;
    RequestBody += "\" end=\"";
    RequestBody += p_EndTime;
    RequestBody += "\"/>\n"
        "      </C:comp-filter>\n"
        "    </C:comp-filter>\n"
        "  </C:filter>\n"
        "</C:calendar-query>";
    esp_http_client_set_post_field(HTTP_Client, RequestBody.c_str(), RequestBody.length());

    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);

    esp_http_client_cleanup(HTTP_Client);

    if ((Error != ESP_OK) || (StatusCode != 207)) {
        ESP_LOGE(TAG, "CalDAV-Query failed: %d (Status: %d)!", Error, StatusCode);
        if (Response.Buffer) {
            free(Response.Buffer);
        }

        return CALDAV_ERROR_HTTP;
    }

    ESP_LOGD(TAG, "CalDAV response received");
    if (Response.Buffer) {
        int EventCount = 0;
        int CurrentEvent = 0;
        char *p_Pos = Response.Buffer;

        /* Count events in the response */
        while ((p_Pos = strstr(p_Pos, "BEGIN:VEVENT")) != NULL) {
            EventCount++;
            p_Pos += 12;
        }

        ESP_LOGD(TAG, "Found: %d events", EventCount);

        if (EventCount == 0) {
            *Length = 0;
            *p_Events = NULL;

            free(Response.Buffer);

            return CALDAV_ERROR_OK;
        }

        /* Allocate memory for all events */
        *p_Events = (CalDAV_Calendar_Event_t *)calloc(EventCount, sizeof(CalDAV_Calendar_Event_t));
        if (*p_Events == NULL) {
            free(Response.Buffer);

            return CALDAV_ERROR_NO_MEM;
        }

        /* Parse each event */
        p_Pos = Response.Buffer;

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
            EventData = (char *)malloc(EventLen + 1);
            if (!EventData) {
                /* Allocation failure for event data: clean up and return */
                for (int i = 0; i < CurrentEvent; ++i) {
                    free((*p_Events)[i].Summary);
                    free((*p_Events)[i].Description);
                    free((*p_Events)[i].Location);
                    free((*p_Events)[i].UID);
                    free((*p_Events)[i].StartTime);
                    free((*p_Events)[i].EndTime);
                }
                free(*p_Events);
                *p_Events = NULL;
                *Length = 0;
                free(Response.Buffer);
                return CALDAV_ERROR_NO_MEM;
            }

            memcpy(EventData, p_Pos, EventLen);
            EventData[EventLen] = '\0';

            /* Parse iCal fields */
            (*p_Events)[CurrentEvent].Summary = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "SUMMARY:"));
            (*p_Events)[CurrentEvent].Description = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "DESCRIPTION:"));
            (*p_Events)[CurrentEvent].Location = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "LOCATION:"));
            (*p_Events)[CurrentEvent].UID = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "UID:"));
            (*p_Events)[CurrentEvent].StartTime = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "DTSTART"));
            (*p_Events)[CurrentEvent].EndTime = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "DTEND"));

            /* Verify allocations for all fields */
            if (((*p_Events)[CurrentEvent].Summary == NULL) ||
                ((*p_Events)[CurrentEvent].Description == NULL) ||
                ((*p_Events)[CurrentEvent].Location == NULL) ||
                ((*p_Events)[CurrentEvent].UID == NULL) ||
                ((*p_Events)[CurrentEvent].StartTime == NULL) ||
                ((*p_Events)[CurrentEvent].EndTime == NULL)) {
                /* Free any fields allocated for the current event */
                free((*p_Events)[CurrentEvent].Summary);
                free((*p_Events)[CurrentEvent].Description);
                free((*p_Events)[CurrentEvent].Location);
                free((*p_Events)[CurrentEvent].UID);
                free((*p_Events)[CurrentEvent].StartTime);
                free((*p_Events)[CurrentEvent].EndTime);

                /* Free all previously parsed events */
                for (int i = 0; i < CurrentEvent; ++i) {
                    free((*p_Events)[i].Summary);
                    free((*p_Events)[i].Description);
                    free((*p_Events)[i].Location);
                    free((*p_Events)[i].UID);
                    free((*p_Events)[i].StartTime);
                    free((*p_Events)[i].EndTime);
                }

                *p_Events = NULL;
                *Length = 0;

                free(*p_Events);
                free(EventData);
                free(Response.Buffer);

                return CALDAV_ERROR_NO_MEM;
            }

            /* Log event details */
            if ((*p_Events)[CurrentEvent].Summary) {
                ESP_LOGD(TAG, "Event %d: %s", CurrentEvent + 1, (*p_Events)[CurrentEvent].Summary);
            }

            CurrentEvent++;
            free(EventData);

            p_Pos = EventEnd + 10;
        }

        *Length = CurrentEvent;
        free(Response.Buffer);
    } else {
        *Length = 0;
        *p_Events = NULL;
    }

    return CALDAV_ERROR_OK;
}

CalDAV_Error_t CalDAV_Calendar_Event_Get(CalDAV_Client_t *p_Client,
                                         const char *EventPath,
                                         CalDAV_Calendar_Event_t *p_Event)
{
    char url[512];
    esp_err_t Error;
    int StatusCode;
    http_response_buffer_t Response;
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->IsInitialized == false) || (EventPath == NULL) || (p_Event == NULL)) {
        return CALDAV_ERROR_INVALID_ARG;
    }

    memset(&Response, 0, sizeof(Response));

    snprintf(url, sizeof(url), "%s%s%s",
             p_Client->ServerURL.c_str(),
             (p_Client->CalendarPath.empty() == false) ? p_Client->CalendarPath.c_str() : "/",
             EventPath);

    _CalDAV_HTTP_Config.timeout_ms = p_Client->TimeoutMs;
    _CalDAV_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDAV_HTTP_Config.url = url;
    _CalDAV_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDAV_HTTP_Config);
    if (HTTP_Client == NULL) {
        return CALDAV_ERROR_FAIL;
    }

    esp_http_client_set_username(HTTP_Client, p_Client->Username.c_str());
    esp_http_client_set_password(HTTP_Client, p_Client->Password.c_str());
    esp_http_client_set_authtype(HTTP_Client, HTTP_AUTH_TYPE_BASIC);

    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);

    esp_http_client_cleanup(HTTP_Client);

    if ((Error != ESP_OK) || (StatusCode != 200)) {
        ESP_LOGE(TAG, "Event download failed: %d (Status: %d)!", Error, StatusCode);
        if (Response.Buffer) {
            free(Response.Buffer);
        }

        return CALDAV_ERROR_HTTP;
    }

    if (Response.Buffer) {
        ESP_LOGD(TAG, "Event data received:");
        ESP_LOGD(TAG, "%s", Response.Buffer);
        /* TODO: Parse iCal format */
        free(Response.Buffer);
    }

    ESP_LOGW(TAG, "iCal parsing not yet implemented");

    return CALDAV_ERROR_OK;
}

void CalDAV_Calendars_Free(CalDAV_Calendar_t *p_Calendars, size_t Length)
{
    if (p_Calendars == NULL) {
        return;
    }

    for (size_t i = 0; i < Length; i++) {
        if (p_Calendars[i].Name) {
            free(p_Calendars[i].Name);
        }

        if (p_Calendars[i].Path) {
            free(p_Calendars[i].Path);
        }

        if (p_Calendars[i].DisplayName) {
            free(p_Calendars[i].DisplayName);
        }

        if (p_Calendars[i].Description) {
            free(p_Calendars[i].Description);
        }

        if (p_Calendars[i].Color) {
            free(p_Calendars[i].Color);
        }
    }
    free(p_Calendars);
}

void CalDAV_Events_Free(CalDAV_Calendar_Event_t *p_Events, size_t Length)
{
    if (p_Events == NULL) {
        return;
    }

    for (size_t i = 0; i < Length; i++) {
        if (p_Events[i].UID) {
            free(p_Events[i].UID);
        }

        if (p_Events[i].Summary) {
            free(p_Events[i].Summary);
        }

        if (p_Events[i].Description) {
            free(p_Events[i].Description);
        }

        if (p_Events[i].StartTime) {
            free(p_Events[i].StartTime);
        }

        if (p_Events[i].EndTime) {
            free(p_Events[i].EndTime);
        }

        if (p_Events[i].Location) {
            free(p_Events[i].Location);
        }
    }

    free(p_Events);
}
