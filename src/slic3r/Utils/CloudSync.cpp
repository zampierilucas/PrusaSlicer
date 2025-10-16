///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CloudSync.hpp"
#include "libslic3r/AppConfig.hpp"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/string.h>

#include <sstream>
#include <iostream>
#include <ctime>
#include <iomanip>

namespace Slic3r {

// Parse HTTP date format (RFC 2616/RFC 1123)
// Example: "Mon, 15 Oct 2025 23:03:45 GMT"
static time_t parse_http_date(const std::string &date_str) {
    if (date_str.empty()) {
        return 0;
    }

    std::tm tm = {};
    std::istringstream ss(date_str);

    // Try RFC 1123 format: "Day, DD Mon YYYY HH:MM:SS GMT"
    ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S");

    if (ss.fail()) {
        // Try alternate format without day name: "DD Mon YYYY HH:MM:SS GMT"
        ss.clear();
        ss.str(date_str);
        ss >> std::get_time(&tm, "%d %b %Y %H:%M:%S");
    }

    if (ss.fail()) {
        return 0; // Failed to parse
    }

    // Convert to time_t (assuming GMT/UTC)
    #ifdef _WIN32
        return _mkgmtime(&tm);
    #else
        return timegm(&tm);
    #endif
}

CloudSyncProvider::~CloudSyncProvider() {}

std::unique_ptr<CloudSyncProvider> CloudSyncProvider::create_from_config(const AppConfig &config)
{
    std::string provider_type = config.get("cloud_sync", "provider_type");
    std::string url = config.get("cloud_sync", "url");
    std::string username = config.get("cloud_sync", "username");
    std::string password = config.get("cloud_sync", "password");

    if (provider_type == "webdav" && !url.empty()) {
        return std::make_unique<WebDAVCloudSync>(url, username, password);
    }

    return nullptr;
}

// WebDAVCloudSync implementation

WebDAVCloudSync::WebDAVCloudSync(const std::string &url, const std::string &username, const std::string &password)
{
    m_url = url;
    m_username = username;
    m_password = password;

    // Ensure URL doesn't end with slash
    if (!m_url.empty() && m_url.back() == '/') {
        m_url.pop_back();
    }
}

std::string WebDAVCloudSync::make_url(const std::string &path) const
{
    std::string url = m_url;
    if (!path.empty()) {
        if (path.front() != '/') {
            url += "/";
        }
        url += path;
    }
    return url;
}

Http WebDAVCloudSync::create_http_request(const std::string &url) const
{
    auto http = Http::get(url);
    if (!m_username.empty()) {
        http.auth_basic(m_username, m_password);
    }
    return http;
}

bool WebDAVCloudSync::is_http_success(unsigned status)
{
    return status >= 200 && status < 300;
}

Http::ErrorFn WebDAVCloudSync::create_error_handler(bool &success, ErrorFn error_fn) const
{
    return [&success, error_fn](std::string body, std::string error, unsigned status) {
        if (error_fn) {
            wxString err_msg = wxString::FromUTF8((error.empty() ? body : error).c_str());
            error_fn(err_msg);
        }
        success = false;
    };
}

std::string WebDAVCloudSync::extract_filename(const std::string &path)
{
    size_t last_slash = path.find_last_of('/');
    if (last_slash != std::string::npos) {
        return path.substr(last_slash + 1);
    }
    return path;
}

bool WebDAVCloudSync::test_connection(wxString &error_msg) const
{
    std::cerr << "[CloudSync] Testing connection to: " << m_url << std::endl;
    bool success = false;
    std::string error_str;

    auto http = create_http_request(m_url);

    http.on_error([&](std::string body, std::string error, unsigned status) {
        std::cerr << "[CloudSync] Connection test failed - Status: " << status << ", Error: " << error << std::endl;
        error_str = error.empty() ? body : error;
        success = false;
    })
    .on_complete([&](std::string body, unsigned status) {
        success = (status >= 200 && status < 400);  // More permissive for connection test
        std::cerr << "[CloudSync] Connection test complete - Status: " << status << ", Success: " << success << std::endl;
    });

    http.perform_sync();

    if (!success) {
        error_msg = wxString::FromUTF8(error_str.c_str());
    }

    return success;
}

bool WebDAVCloudSync::upload_file(const boost::filesystem::path &local_path,
                                  const std::string &remote_path,
                                  ProgressFn progress_fn,
                                  ErrorFn error_fn) const
{
    std::cerr << "[CloudSync] Uploading: " << local_path.string() << " -> " << remote_path << std::endl;
    bool success = false;
    std::string url = make_url(remote_path);
    std::cerr << "[CloudSync] PUT URL: " << url << std::endl;

    // Note: PUT requires special handling, can't use create_http_request helper
    auto http = Http::put(url);
    if (!m_username.empty()) {
        http.auth_basic(m_username, m_password);
    }

    http.set_put_body(local_path)
        .on_error(create_error_handler(success, error_fn))
        .on_complete([&](std::string body, unsigned status) {
            success = is_http_success(status);
        });

    if (progress_fn) {
        http.on_progress([&](Http::Progress progress, bool &cancel) {
            progress_fn(progress, cancel);
        });
    }

    http.perform_sync();

    return success;
}

bool WebDAVCloudSync::download_file(const std::string &remote_path,
                                   const boost::filesystem::path &local_path,
                                   ProgressFn progress_fn,
                                   ErrorFn error_fn) const
{
    std::cerr << "[CloudSync] Downloading: " << remote_path << " -> " << local_path.string() << std::endl;
    bool success = false;
    std::string url = make_url(remote_path);
    std::string content;

    auto http = create_http_request(url);

    http.on_error(create_error_handler(success, error_fn))
        .on_complete([&](std::string body, unsigned status) {
            if (is_http_success(status)) {
                content = body;
                success = true;
            }
        });

    if (progress_fn) {
        http.on_progress([&](Http::Progress progress, bool &cancel) {
            progress_fn(progress, cancel);
        });
    }

    http.perform_sync();

    if (success && !content.empty()) {
        // Write content to local file
        boost::nowide::ofstream file(local_path.string(), std::ios::binary);
        if (file.is_open()) {
            file.write(content.c_str(), content.size());
            file.close();
        } else {
            success = false;
            if (error_fn) {
                error_fn(wxString::FromUTF8("Failed to write local file"));
            }
        }
    }

    return success;
}

bool WebDAVCloudSync::list_files(const std::string &remote_path,
                                std::vector<CloudFile> &files,
                                ErrorFn error_fn) const
{
    bool success = false;
    std::string url = make_url(remote_path);
    std::cerr << "[CloudSync] PROPFIND: " << url << std::endl;
    std::string response_body;

    // WebDAV PROPFIND request
    std::string propfind_body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<D:propfind xmlns:D=\"DAV:\">"
        "  <D:prop>"
        "    <D:displayname/>"
        "    <D:getcontentlength/>"
        "    <D:getlastmodified/>"
        "    <D:resourcetype/>"
        "    <D:getetag/>"
        "  </D:prop>"
        "</D:propfind>";

    auto http = create_http_request(url);

    http.custom_request("PROPFIND")
        .header("Depth", "1")
        .header("Content-Type", "application/xml; charset=utf-8")
        .set_post_body(propfind_body)
        .on_error(create_error_handler(success, error_fn))
        .on_complete([&](std::string body, unsigned status) {
            if (is_http_success(status)) {
                response_body = body;
                success = true;
            }
        });

    http.perform_sync();

    if (success) {
        success = parse_propfind_response(response_body, files);
        if (!success && error_fn) {
            error_fn(wxString::FromUTF8("Failed to parse WebDAV response"));
        }
    }

    return success;
}

bool WebDAVCloudSync::parse_propfind_response(const std::string &xml_response,
                                             std::vector<CloudFile> &files) const
{
    try {
        std::istringstream is(xml_response);
        boost::property_tree::ptree pt;
        boost::property_tree::read_xml(is, pt);

        // Parse WebDAV multistatus response
        for (const auto &response : pt.get_child("D:multistatus")) {
            if (response.first != "D:response") continue;

            CloudFile file;

            auto href = response.second.get_optional<std::string>("D:href");
            if (!href) continue;

            // Strip the base URL from href if present
            // href might be absolute like "/storage/path/PrusaSlicer/print/file.ini"
            // or relative like "file.ini"
            std::string href_str = *href;

            // Extract the path component (remove protocol and domain if present)
            size_t path_start = href_str.find("://");
            if (path_start != std::string::npos) {
                path_start = href_str.find('/', path_start + 3);
                if (path_start != std::string::npos) {
                    href_str = href_str.substr(path_start);
                }
            }

            // Now remove the base URL path from the beginning if present
            // m_url is like "https://example.com/storage/path"
            // We need to extract "/storage/path" from it
            std::string url_path;
            size_t url_path_start = m_url.find("://");
            if (url_path_start != std::string::npos) {
                url_path_start = m_url.find('/', url_path_start + 3);
                if (url_path_start != std::string::npos) {
                    url_path = m_url.substr(url_path_start);
                }
            }

            // Remove url_path from beginning of href_str if present
            if (!url_path.empty() && href_str.find(url_path) == 0) {
                href_str = href_str.substr(url_path.length());
            }

            file.path = href_str;
            file.name = extract_filename(file.path);

            auto propstat = response.second.get_child_optional("D:propstat");
            if (!propstat) continue;

            auto prop = propstat->get_child_optional("D:prop");
            if (!prop) continue;

            // Get file size
            auto size = prop->get_optional<size_t>("D:getcontentlength");
            if (size) {
                file.size = *size;
            }

            // Check if it's a directory
            auto resourcetype = prop->get_child_optional("D:resourcetype");
            file.is_directory = (resourcetype && resourcetype->get_child_optional("D:collection"));

            // Get modification time
            auto lastmod = prop->get_optional<std::string>("D:getlastmodified");
            if (lastmod) {
                file.modified_time = parse_http_date(*lastmod);
            }

            files.push_back(file);
        }

        return true;
    } catch (const std::exception &e) {
        return false;
    }
}

bool WebDAVCloudSync::delete_file(const std::string &remote_path,
                                 ErrorFn error_fn) const
{
    std::cerr << "[CloudSync] Deleting file: " << remote_path << std::endl;
    bool success = false;
    std::string url = make_url(remote_path);

    auto http = create_http_request(url);

    http.custom_request("DELETE")
        .on_error([&](std::string body, std::string error, unsigned status) {
            std::cerr << "[CloudSync] DELETE failed - Status: " << status << ", Error: " << error << std::endl;
            if (error_fn) {
                wxString err_msg = wxString::FromUTF8((error.empty() ? body : error).c_str());
                error_fn(err_msg);
            }
            success = false;
        })
        .on_complete([&](std::string body, unsigned status) {
            success = is_http_success(status);
            std::cerr << "[CloudSync] DELETE complete - Status: " << status << ", Success: " << success << std::endl;
        });

    http.perform_sync();

    return success;
}

bool WebDAVCloudSync::create_directory(const std::string &remote_path,
                                      ErrorFn error_fn) const
{
    std::cerr << "[CloudSync] Creating directory: " << remote_path << std::endl;
    bool success = false;
    std::string url = make_url(remote_path);
    std::cerr << "[CloudSync] Full URL: " << url << std::endl;

    auto http = create_http_request(url);

    http.custom_request("MKCOL")
        .on_error([&](std::string body, std::string error, unsigned status) {
            // 405 (Method Not Allowed) may be returned when directory already exists
            if (status != 405) {
                std::cerr << "[CloudSync] MKCOL failed - Status: " << status << ", Error: " << error << std::endl;
                if (error_fn) {
                    wxString err_msg = wxString::FromUTF8((error.empty() ? body : error).c_str());
                    error_fn(err_msg);
                }
                success = false;
            } else {
                // Directory already exists - treat as success
                success = true;
            }
        })
        .on_complete([&](std::string body, unsigned status) {
            success = is_http_success(status) || status == 405; // 405 if already exists
            std::cerr << "[CloudSync] MKCOL complete - Status: " << status << ", Success: " << success << std::endl;
        });

    http.perform_sync();

    return success;
}

} // namespace Slic3r
