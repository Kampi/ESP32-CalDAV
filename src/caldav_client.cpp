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

#if CONFIG_ESP32_CALDAV_USE_PSRAM
    #define CUSTOM_MALLOC(Ptr)              heap_caps_malloc(Ptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    #define CUSTOM_CALLOC(Num, Size)        heap_caps_calloc(Num, Size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    #define CUSTOM_REALLOC(Ptr, NewSize)    heap_caps_realloc(Ptr, NewSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
    #define CUSTOM_FREE(Ptr)                heap_caps_free(Ptr)
#else
    #define CUSTOM_MALLOC(Ptr)              malloc(Ptr)
    #define CUSTOM_CALLOC(Num, Size)        calloc(Num, Size)
    #define CUSTOM_REALLOC(Ptr, NewSize)    realloc(Ptr, NewSize)
    #define CUSTOM_FREE(Ptr)                free(Ptr)
#endif

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

/** @brief Structure holding configuration and state for a CalDAV client,
 *         including server URL, credentials, calendar path, timeout, and
 *         initialization status.
 */
static thread_local esp_http_client_config_t _CalDAV_HTTP_Config;

/** @brief      Helper function to convert std::string to char*.
 *  @param str  The string to convert
 *  @return     Allocated char* or NULL if string is empty
 */
static inline char *_CalDAV_String_to_cstr(const std::string &str)
{
    return str.empty() ? NULL : strdup(str.c_str());
}

/** @brief  Buffer used to accumulate HTTP response data.
 *          This structure holds a dynamically allocated character buffer, its current
 *          total size, and the write position used while assembling the complete
 *          HTTP response body in the esp_http_client event callback.
 */
typedef struct {
    char *Buffer;
    size_t Size;
    size_t Position;
} HTTP_Response_Buffer_t;

/** @brief          HTTP Event Handler.
 *  @param p_Event  Pointer to HTTP Event
 *  @return         ESP_OK on success
 */
static esp_err_t on_HTTP_Event_Handler(esp_http_client_event_t *p_Event)
{
    HTTP_Response_Buffer_t *p_OutputBuffer = (HTTP_Response_Buffer_t *)p_Event->user_data;

    switch (p_Event->event_id) {
        case HTTP_EVENT_ON_DATA: {
            /* Allocate buffer if not yet available */
            if (p_OutputBuffer->Buffer == NULL) {
                p_OutputBuffer->Buffer = (char *)CUSTOM_MALLOC(CONFIG_ESP32_CALDAV_BUFFER_LENGTH);
                p_OutputBuffer->Size = CONFIG_ESP32_CALDAV_BUFFER_LENGTH;
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

                char *p_NewBuffer = (char *)CUSTOM_REALLOC(p_OutputBuffer->Buffer, NewSize);
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
    size_t ColonPos;
    std::string Value;

    if (Start == std::string::npos) {
        return std::string();
    }

    Start += Field.length();

    ColonPos = Data.find(':', Start);
    if (ColonPos != std::string::npos) {
        /* Check if colon is before the line end */
        size_t LineEnd = Data.find_first_of("\n\r", Start);
        if ((LineEnd == std::string::npos) || (ColonPos < LineEnd)) {
            Start = ColonPos + 1;
        }
    }

    End = Data.find_first_of("\n\r", Start);
    if (End == std::string::npos) {
        return Data.substr(Start);
    }

    /* Remove possible \r at the end */
    Value = Data.substr(Start, End - Start);
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

CalDAV_Error_t CalDAV_Client_Init(const CalDAV_Config_t *p_Config, CalDAV_Client_t *p_Client)
{
    if ((p_Config == NULL) || (p_Config->ServerURL[0] == '\0') || (p_Config->Username[0] == '\0') ||
        (p_Config->Password[0] == '\0')) {
        ESP_LOGE(TAG, "Invalid configuration!");

        return CALDAV_ERROR_INVALID_ARG;
    }

    /* Initialize HTTP config completely to avoid garbage values */
    memset(&_CalDAV_HTTP_Config, 0, sizeof(_CalDAV_HTTP_Config));

    _CalDAV_HTTP_Config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    _CalDAV_HTTP_Config.crt_bundle_attach = esp_crt_bundle_attach;
    _CalDAV_HTTP_Config.event_handler = on_HTTP_Event_Handler;
    _CalDAV_HTTP_Config.username = p_Config->Username;
    _CalDAV_HTTP_Config.password = p_Config->Password;
    _CalDAV_HTTP_Config.auth_type = HTTP_AUTH_TYPE_BASIC;
    _CalDAV_HTTP_Config.timeout_ms = p_Config->TimeoutMs;

    p_Client->ServerURL = std::string(p_Config->ServerURL);
    p_Client->IsInitialized = true;

    ESP_LOGD(TAG, "CalDAV client initialized: %s", p_Config->ServerURL);

    return CALDAV_ERROR_OK;
}

void CalDAV_Client_Deinit(CalDAV_Client_t *p_Client)
{
    if ((p_Client == NULL) || (p_Client->IsInitialized == false)) {
        return;
    }

    p_Client->IsInitialized = false;
}

CalDAV_Error_t CalDAV_Test_Connection(CalDAV_Client_t *p_Client)
{
    esp_err_t Error;
    int StatusCode;
    HTTP_Response_Buffer_t Response;
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->IsInitialized == false)) {
        return CALDAV_ERROR_NOT_INITIALIZED;
    }

    memset(&Response, 0, sizeof(Response));

    _CalDAV_HTTP_Config.url = p_Client->ServerURL.c_str();
    _CalDAV_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDAV_HTTP_Config);
    if (HTTP_Client == NULL) {
        ESP_LOGE(TAG, "HTTP client initialization failed!");

        return CALDAV_ERROR_FAIL;
    }

    esp_http_client_set_header(HTTP_Client, "Depth", "0");
    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);
    esp_http_client_cleanup(HTTP_Client);

    if (Response.Buffer) {
        CUSTOM_FREE(Response.Buffer);
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
                                     CalDAV_Calendar_List_t *p_Calendars)
{
    int StatusCode;
    esp_err_t Error;
    HTTP_Response_Buffer_t Response;
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->IsInitialized == false) || (p_Calendars == NULL)) {
        return CALDAV_ERROR_INVALID_ARG;
    }

    memset(&Response, 0, sizeof(Response));

    _CalDAV_HTTP_Config.url = p_Client->ServerURL.c_str();
    _CalDAV_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDAV_HTTP_Config);
    if (HTTP_Client == NULL) {
        return CALDAV_ERROR_FAIL;
    }

    esp_http_client_set_header(HTTP_Client, "Content-Type", "application/xml; charset=utf-8");
    esp_http_client_set_header(HTTP_Client, "Depth", "1");
    esp_http_client_set_method(HTTP_Client, HTTP_METHOD_PROPFIND);
    esp_http_client_set_post_field(HTTP_Client, _CalDAV_Propfind_Body, strlen(_CalDAV_Propfind_Body));
    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);
    esp_http_client_cleanup(HTTP_Client);

    if (Error != ESP_OK) {
        ESP_LOGE(TAG, "Calendar PROPFIND failed: %d!", Error);
        if (Response.Buffer) {
            CUSTOM_FREE(Response.Buffer);
        }

        return CALDAV_ERROR_HTTP;
    }

    if ((StatusCode != 200) && (StatusCode != 207)) {
        ESP_LOGE(TAG, "Calendar PROPFIND unexpected status: %d!", StatusCode);
        if (Response.Buffer) {
            CUSTOM_FREE(Response.Buffer);
        }

        return CALDAV_ERROR_HTTP;
    }

    ESP_LOGD(TAG, "Calendar response received");
    ESP_LOGD(TAG, "Response buffer: %s, position: %zu", Response.Buffer ? "available" : "NULL", Response.Position);

    if (Response.Buffer) {
        int CalendarCount = 0;
        int CurrentCalendar = 0;
        size_t SearchPos = 0;

        ESP_LOGD(TAG, "PROPFIND Response (first 500 chars): %.*s", 500, Response.Buffer);

        std::string ResponseStr(Response.Buffer);
        CUSTOM_FREE(Response.Buffer);

        /* Check for HTML response (indicates error) */
        if (ResponseStr.find("<!DOCTYPE html>") != std::string::npos ||
            ResponseStr.find("<html>") != std::string::npos ||
            ResponseStr.find("<html ") != std::string::npos) {
            ESP_LOGE(TAG, "Invalid XML!");

            p_Calendars->Length = 0;
            p_Calendars->Calendar = NULL;

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
                    } else {
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
            p_Calendars->Length = 0;
            p_Calendars->Calendar = NULL;

            return CALDAV_ERROR_OK;
        }

        /* Allocate memory for all calendars */
        p_Calendars->Calendar = (CalDAV_Calendar_t *)CUSTOM_CALLOC(CalendarCount, sizeof(CalDAV_Calendar_t));
        if (p_Calendars->Calendar == NULL) {
            p_Calendars->Length = 0;
            p_Calendars->Calendar = NULL;

            return CALDAV_ERROR_NO_MEM;
        }

        /* Parse each calendar response block */
        SearchPos = 0;

        std::string PrincipalPath;
        while (((SearchPos = ResponseStr.find("<response>", SearchPos)) != std::string::npos) &&
               (CurrentCalendar < CalendarCount)) {
            size_t ResponseEnd = ResponseStr.find("</response>", SearchPos);
            if (ResponseEnd == std::string::npos) {
                break;
            }

            /* Extract this response block */
            std::string ResponseBlock = ResponseStr.substr(SearchPos, ResponseEnd - SearchPos);

            /* Extract and log href for debugging */
            std::string HrefStr = _CalDAV_Extract_XML_Tag_Value(ResponseBlock, "href");
            ESP_LOGD(TAG, "Response href: %s", HrefStr.c_str());

            /* Check if this response contains a calendar (not just a collection) */
            /* Must have <resourcetype><calendar/> tag to be a real calendar */
            bool isCalendar = false;
            size_t ResTypePos = ResponseBlock.find("<resourcetype>");
            if (ResTypePos != std::string::npos) {
                size_t ResTypeEnd = ResponseBlock.find("</resourcetype>", ResTypePos);
                if (ResTypeEnd != std::string::npos) {
                    std::string ResTypeBlock = ResponseBlock.substr(ResTypePos, ResTypeEnd - ResTypePos);
                    ESP_LOGD(TAG, "ResourceType block: %s", ResTypeBlock.c_str());
                    if ((ResTypeBlock.find(":calendar") != std::string::npos) || 
                        (ResTypeBlock.find("<calendar") != std::string::npos)) {
                        isCalendar = true;
                        ESP_LOGD(TAG, "  -> Is a calendar!");
                    } else if (ResTypeBlock.find("<principal") != std::string::npos) {
                        /* Store principal path for recursive search */
                        if (PrincipalPath.empty() && (HrefStr.empty() == false)) {
                            PrincipalPath = HrefStr;
                            ESP_LOGD(TAG, "  -> Found principal path: %s", PrincipalPath.c_str());
                        }
                    } else {
                        ESP_LOGD(TAG, "  -> Not a calendar (only collection)");
                    }
                }
            }
            
            if (isCalendar) {
                ESP_LOGD(TAG, "Processing calendar response block");

                /* Extract href (path) */
                std::string HrefStr = _CalDAV_Extract_XML_Tag_Value(ResponseBlock, "href");
                if (HrefStr.empty() == false) {
                    p_Calendars->Calendar[CurrentCalendar].Path = _CalDAV_String_to_cstr(HrefStr);
                    ESP_LOGD(TAG, "  href: %s", HrefStr.c_str());

                    /* Extract calendar name from path */
                    size_t LastSlashPos = HrefStr.find_last_of('/');
                    if (LastSlashPos != std::string::npos) {
                        if ((LastSlashPos + 1) < HrefStr.length()) {
                            /* Name after last slash */
                            p_Calendars->Calendar[CurrentCalendar].Name = strdup(HrefStr.substr(LastSlashPos + 1).c_str());
                        } else if (LastSlashPos > 0) {
                            /* Path ends with /, take previous segment */
                            size_t PrevSlashPos = HrefStr.find_last_of('/', LastSlashPos - 1);
                            if (PrevSlashPos != std::string::npos) {
                                std::string NamePart = HrefStr.substr(PrevSlashPos + 1, LastSlashPos - PrevSlashPos - 1);
                                p_Calendars->Calendar[CurrentCalendar].Name = strdup(NamePart.c_str());
                            }
                        }
                    }
                }

                p_Calendars->Calendar[CurrentCalendar].DisplayName = _CalDAV_String_to_cstr(
                                                                         _CalDAV_Extract_XML_Tag_Value(ResponseBlock, "displayname"));
                p_Calendars->Calendar[CurrentCalendar].Description = _CalDAV_String_to_cstr(
                                                                         _CalDAV_Extract_XML_Tag_Value(ResponseBlock, "calendar-description"));

                ESP_LOGD(TAG, "Calendar %d:", CurrentCalendar + 1);
                if (p_Calendars->Calendar[CurrentCalendar].Name) {
                    ESP_LOGD(TAG, "  Name: %s", p_Calendars->Calendar[CurrentCalendar].Name);
                }

                if (p_Calendars->Calendar[CurrentCalendar].DisplayName) {
                    ESP_LOGD(TAG, "  Display name: %s", p_Calendars->Calendar[CurrentCalendar].DisplayName);
                }

                if (p_Calendars->Calendar[CurrentCalendar].Path) {
                    ESP_LOGD(TAG, "  Path: %s", p_Calendars->Calendar[CurrentCalendar].Path);
                }

                CurrentCalendar++;
            }

            SearchPos = ResponseEnd + 1;
        }

        p_Calendars->Length = CurrentCalendar;

        /* If no calendars found but we found a principal, search in the principal path */
        if ((CurrentCalendar == 0) && (PrincipalPath.empty() == false)) {
            ESP_LOGD(TAG, "No calendars found, searching in principal path: %s", PrincipalPath.c_str());

            /* Build full URL with principal path */
            /* Principal path is absolute from server root, so extract scheme://host */
            std::string BaseURL = p_Client->ServerURL;
            size_t SchemeEnd = BaseURL.find("://");
            if (SchemeEnd != std::string::npos) {
                size_t PathStart = BaseURL.find('/', SchemeEnd + 3);
                if (PathStart != std::string::npos) {
                    BaseURL = BaseURL.substr(0, PathStart);
                }
            }

            ESP_LOGD(TAG, "Constructed principal URL: %s",  (BaseURL + PrincipalPath).c_str());

            /* Create temporary client with principal URL */
            CalDAV_Client_t TempClient = *p_Client;
            TempClient.ServerURL = BaseURL + PrincipalPath;

            /* Recursive call to search in principal path */
            return CalDAV_Calendars_List(&TempClient, p_Calendars);
        }
    } else {
        p_Calendars->Length = 0;
        p_Calendars->Calendar = NULL;
    }

    return CALDAV_ERROR_OK;
}

CalDAV_Error_t CalDAV_Calendar_Find_By_Name(const CalDAV_Calendar_List_t *p_Calendars,
                                            const char *p_Name,
                                            CalDAV_Calendar_t **pp_Calendar)
{
    if ((p_Calendars == NULL) || (p_Name == NULL) || (pp_Calendar == NULL)) {
        return CALDAV_ERROR_INVALID_ARG;
    }

    *pp_Calendar = NULL;

    /* Search through all calendars */
    for (size_t i = 0; i < p_Calendars->Length; i++) {
        bool Found = false;

        /* Check Name field */
        if (p_Calendars->Calendar[i].Name != NULL) {
            if (strcmp(p_Calendars->Calendar[i].Name, p_Name) == 0) {
                Found = true;
            }
        }

        /* Check DisplayName field if not found yet */
        if ((Found == false) && (p_Calendars->Calendar[i].DisplayName != NULL)) {
            if (strcmp(p_Calendars->Calendar[i].DisplayName, p_Name) == 0) {
                Found = true;
            }
        }

        if (Found) {
            *pp_Calendar = &p_Calendars->Calendar[i];
            ESP_LOGD(TAG, "Found calendar: %s (Path: %s)", 
                     p_Name, 
                     p_Calendars->Calendar[i].Path ? p_Calendars->Calendar[i].Path : "Unknown");

            return CALDAV_ERROR_OK;
        }
    }

    ESP_LOGW(TAG, "Calendar '%s' not found in list of %zu calendars", p_Name, p_Calendars->Length);

    return CALDAV_ERROR_NOT_FOUND;
}

CalDAV_Error_t CalDAV_Calendar_Events_List(CalDAV_Client_t *p_Client,
                                           CalDAV_Calendar_Event_t **p_Events,
                                           size_t *Length,
                                           const char *p_CalendarPath,
                                           const struct tm* p_StartTime,
                                           const struct tm* p_EndTime)
{
    char URL[512];
    char StartTimeString[20];
    char EndTimeString[20];
    esp_err_t Error;
    int StatusCode;
    HTTP_Response_Buffer_t Response;
    esp_http_client_handle_t HTTP_Client;

    if ((p_Client == NULL) || (p_Client->IsInitialized == false) || (p_Events == NULL) || (Length == NULL) ||
        (p_CalendarPath == NULL)) {
        return CALDAV_ERROR_INVALID_ARG;
    }

    memset(&Response, 0, sizeof(Response));
    memset(StartTimeString, 0, sizeof(StartTimeString));
    memset(EndTimeString, 0, sizeof(EndTimeString));
    memset(URL, 0, sizeof(URL));

    /* Format as CalDAV expects: YYYYMMDDTHHMMSSZ */
    strftime(StartTimeString, sizeof(StartTimeString), "%Y%m%dT%H%M%SZ", p_StartTime);
    strftime(EndTimeString, sizeof(EndTimeString), "%Y%m%dT%H%M%SZ", p_EndTime);

    /* Build URL - if path is absolute (starts with /), use scheme://host + path */
    if (p_CalendarPath[0] == '/') {
        std::string BaseURL;
        size_t SchemeEnd;

        /* Extract base URL (scheme://host) */
        BaseURL = p_Client->ServerURL;
        SchemeEnd = BaseURL.find("://");
        if (SchemeEnd != std::string::npos) {
            size_t PathStart;
            
            PathStart = BaseURL.find('/', SchemeEnd + 3);
            if (PathStart != std::string::npos) {
                BaseURL = BaseURL.substr(0, PathStart);
            }
        }

        snprintf(URL, sizeof(URL), "%s%s", BaseURL.c_str(), p_CalendarPath);
    } else {
        /* Relative path, append to server URL */
        snprintf(URL, sizeof(URL), "%s/%s", p_Client->ServerURL.c_str(), p_CalendarPath);
    }

    ESP_LOGD(TAG, "Fetching events from %s between %s to %s", URL, StartTimeString, EndTimeString);

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
    RequestBody += StartTimeString;
    RequestBody += "\" end=\"";
    RequestBody += EndTimeString;
    RequestBody += "\"/>\n"
                   "      </C:comp-filter>\n"
                   "    </C:comp-filter>\n"
                   "  </C:filter>\n"
                   "</C:calendar-query>";

    _CalDAV_HTTP_Config.url = URL;
    _CalDAV_HTTP_Config.user_data = &Response;
    HTTP_Client = esp_http_client_init(&_CalDAV_HTTP_Config);
    if (HTTP_Client == NULL) {
        return CALDAV_ERROR_FAIL;
    }

    esp_http_client_set_method(HTTP_Client, HTTP_METHOD_POST);
    esp_http_client_set_header(HTTP_Client, "Content-Type", "application/xml; charset=utf-8");
    esp_http_client_set_header(HTTP_Client, "Depth", "1");
    esp_http_client_set_header(HTTP_Client, "X-HTTP-Method-Override", "REPORT");
    esp_http_client_set_post_field(HTTP_Client, RequestBody.c_str(), RequestBody.length());
    Error = esp_http_client_perform(HTTP_Client);
    StatusCode = esp_http_client_get_status_code(HTTP_Client);
    esp_http_client_cleanup(HTTP_Client);

    if (Error != ESP_OK) {
        ESP_LOGE(TAG, "CalDAV-Query failed: %d (Status: %d)!", Error, StatusCode);
        if (Response.Buffer) {
            ESP_LOGE(TAG, "Response buffer content (first 500 chars): %.*s", 500, Response.Buffer);

            CUSTOM_FREE(Response.Buffer);
        }

        return CALDAV_ERROR_HTTP;
    }

    if ((StatusCode != 200) && (StatusCode != 207)) {
        ESP_LOGE(TAG, "CalDAV-Query unexpected status: %d!", StatusCode);
        if (Response.Buffer) {
            ESP_LOGE(TAG, "Response buffer content (first 500 chars): %.*s", 500, Response.Buffer);

            CUSTOM_FREE(Response.Buffer);
        }

        return CALDAV_ERROR_HTTP;
    }

    if (Response.Buffer) {
        ESP_LOGD(TAG, "CalDAV response (Status: %d, Length: %zu)", StatusCode, Response.Position);
        ESP_LOGD(TAG, "Response content (first 1000 chars): %.*s", 1000, Response.Buffer);
    } else {
        ESP_LOGW(TAG, "CalDAV response buffer is NULL!");
        return CALDAV_ERROR_HTTP;
    }

    int EventCount = 0;
    int CurrentEvent = 0;
    std::string ResponseStr(Response.Buffer);
    size_t SearchPos = 0;

    /* Count events in the response */
    while ((SearchPos = ResponseStr.find("BEGIN:VEVENT", SearchPos)) != std::string::npos) {
        EventCount++;
        SearchPos += 12;
    }

    ESP_LOGD(TAG, "Found: %d events in response", EventCount);
    
    if (EventCount == 0) {
        ESP_LOGD(TAG, "No events found in calendar");
    }

    if (EventCount == 0) {
        *Length = 0;
        *p_Events = NULL;

        CUSTOM_FREE(Response.Buffer);

        return CALDAV_ERROR_OK;
    }

    ESP_LOGD(TAG, "Allocating memory for %d events (%zu bytes)", EventCount, EventCount * sizeof(CalDAV_Calendar_Event_t));

    /* Allocate memory for all events */
    *p_Events = (CalDAV_Calendar_Event_t *)CUSTOM_CALLOC(EventCount, sizeof(CalDAV_Calendar_Event_t));
    if (*p_Events == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for events!");
        CUSTOM_FREE(Response.Buffer);

        return CALDAV_ERROR_NO_MEM;
    }

    ESP_LOGD(TAG, "Memory allocated successfully");

    /* Parse each event */
    SearchPos = 0;

    while ((SearchPos = ResponseStr.find("BEGIN:VEVENT", SearchPos)) != std::string::npos && CurrentEvent < EventCount) {
            size_t EventLen;
            size_t EventEnd;
            char *EventData;

            EventEnd = ResponseStr.find("END:VEVENT", SearchPos);
            if (EventEnd == std::string::npos) {
                break;
            }

            /* Extract event data */
            EventLen = EventEnd - SearchPos + 10;
            EventData = (char *)CUSTOM_MALLOC(EventLen + 1);

            if (EventData == NULL) {
                /* Allocation failure for event data: clean up and return */
                for (int i = 0; i < CurrentEvent; ++i) {
                    CUSTOM_FREE((*p_Events)[i].Summary);
                    CUSTOM_FREE((*p_Events)[i].Description);
                    CUSTOM_FREE((*p_Events)[i].Location);
                    CUSTOM_FREE((*p_Events)[i].UID);
                    CUSTOM_FREE((*p_Events)[i].StartTime);
                    CUSTOM_FREE((*p_Events)[i].EndTime);
                }

                *p_Events = NULL;
                *Length = 0;

                CUSTOM_FREE(*p_Events);
                CUSTOM_FREE(Response.Buffer);

                return CALDAV_ERROR_NO_MEM;
            }

            memcpy(EventData, Response.Buffer + SearchPos, EventLen);
            EventData[EventLen] = '\0';

            /* Parse iCal fields */
            (*p_Events)[CurrentEvent].Summary = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "SUMMARY:"));
            (*p_Events)[CurrentEvent].Description = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "DESCRIPTION:"));
            (*p_Events)[CurrentEvent].Location = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "LOCATION:"));
            (*p_Events)[CurrentEvent].UID = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "UID:"));
            (*p_Events)[CurrentEvent].StartTime = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "DTSTART"));
            (*p_Events)[CurrentEvent].EndTime = _CalDAV_String_to_cstr(_CalDAV_Extract_iCal_Field(EventData, "DTEND"));

            CurrentEvent++;
            CUSTOM_FREE(EventData);

            SearchPos = EventEnd + 10;
    }

    *Length = CurrentEvent;
    CUSTOM_FREE(Response.Buffer);

    return CALDAV_ERROR_OK;
}

void CalDAV_Calendars_Free(CalDAV_Calendar_List_t *p_Calendars)
{
    if (p_Calendars == NULL) {
        return;
    }

    for (size_t i = 0; i < p_Calendars->Length; i++) {
        if (p_Calendars->Calendar[i].Name) {
            CUSTOM_FREE(p_Calendars->Calendar[i].Name);
        }

        if (p_Calendars->Calendar[i].Path) {
            CUSTOM_FREE(p_Calendars->Calendar[i].Path);
        }

        if (p_Calendars->Calendar[i].DisplayName) {
            CUSTOM_FREE(p_Calendars->Calendar[i].DisplayName);
        }

        if (p_Calendars->Calendar[i].Description) {
            CUSTOM_FREE(p_Calendars->Calendar[i].Description);
        }

        if (p_Calendars->Calendar[i].Color) {
            CUSTOM_FREE(p_Calendars->Calendar[i].Color);
        }
    }

    CUSTOM_FREE(p_Calendars->Calendar);
}

void CalDAV_Events_Free(CalDAV_Calendar_Event_t *p_Events, size_t Length)
{
    if (p_Events == NULL) {
        return;
    }

    for (size_t i = 0; i < Length; i++) {
        if (p_Events[i].UID) {
            CUSTOM_FREE(p_Events[i].UID);
        }

        if (p_Events[i].Summary) {
            CUSTOM_FREE(p_Events[i].Summary);
        }

        if (p_Events[i].Description) {
            CUSTOM_FREE(p_Events[i].Description);
        }

        if (p_Events[i].StartTime) {
            CUSTOM_FREE(p_Events[i].StartTime);
        }

        if (p_Events[i].EndTime) {
            CUSTOM_FREE(p_Events[i].EndTime);
        }

        if (p_Events[i].Location) {
            CUSTOM_FREE(p_Events[i].Location);
        }
    }

    CUSTOM_FREE(p_Events);
}
