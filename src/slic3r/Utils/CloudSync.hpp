///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CloudSync_hpp_
#define slic3r_CloudSync_hpp_

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <boost/filesystem/path.hpp>

#include <wx/string.h>
#include "Http.hpp"

namespace Slic3r {

class AppConfig;

// Struct representing a file in cloud storage
struct CloudFile
{
    std::string name;
    std::string path;
    size_t size;
    time_t modified_time;
    bool is_directory;
    std::string hash;  // MD5 hash of file content

    CloudFile() : size(0), modified_time(0), is_directory(false) {}
};

// Utility: Calculate MD5 hash from data, skipping PrusaSlicer timestamp header for deterministic comparison
std::string calculate_config_bundle_hash(const char* data, size_t size);

// WebDAV cloud sync implementation
class CloudSyncWebDAV
{
public:
    typedef Http::ProgressFn ProgressFn;
    typedef std::function<void(wxString /* error */)> ErrorFn;

    CloudSyncWebDAV(const std::string &url, const std::string &username, const std::string &password);
    ~CloudSyncWebDAV() = default;

    // Upload a file to cloud storage
    bool upload_file(const boost::filesystem::path &local_path,
                    const std::string &remote_path,
                    ProgressFn progress_fn,
                    ErrorFn error_fn) const;

    // Download a file from cloud storage
    bool download_file(const std::string &remote_path,
                      const boost::filesystem::path &local_path,
                      ProgressFn progress_fn,
                      ErrorFn error_fn) const;

    // List files in a remote directory
    bool list_files(const std::string &remote_path,
                   std::vector<CloudFile> &files,
                   ErrorFn error_fn) const;

    // Delete a file from cloud storage
    bool delete_file(const std::string &remote_path,
                    ErrorFn error_fn) const;

    // Create a directory in cloud storage
    bool create_directory(const std::string &remote_path,
                         ErrorFn error_fn) const;

private:
    std::string m_url;
    std::string m_username;
    std::string m_password;

    // Helper to construct full URL
    std::string make_url(const std::string &path) const;

    // Helper to create authenticated HTTP request
    Http create_http_request(const std::string &url) const;

    // Helper to check HTTP success status
    static bool is_http_success(unsigned status);

    // Helper to create standardized error handler
    Http::ErrorFn create_error_handler(bool &success, ErrorFn error_fn) const;

    // Helper to parse WebDAV PROPFIND response
    bool parse_propfind_response(const std::string &xml_response,
                                std::vector<CloudFile> &files) const;

    // Helper to extract filename from path
    static std::string extract_filename(const std::string &path);
};

} // namespace Slic3r

#endif // slic3r_CloudSync_hpp_
