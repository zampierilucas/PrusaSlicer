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

    CloudFile() : size(0), modified_time(0), is_directory(false) {}
};

// Struct for tracking deleted files (minimal metadata for deletion log)
struct DeletedFile
{
    std::string remote_path;
    time_t deleted_at;

    DeletedFile() : deleted_at(0) {}
    DeletedFile(const std::string &path, time_t timestamp)
        : remote_path(path), deleted_at(timestamp) {}
};

// Abstract base class for cloud sync providers
class CloudSyncProvider
{
public:
    virtual ~CloudSyncProvider();

    typedef Http::ProgressFn ProgressFn;
    typedef std::function<void(wxString /* error */)> ErrorFn;
    typedef std::function<void(wxString /* message */)> InfoFn;

    virtual const char* get_name() const = 0;

    // Test connection to cloud storage
    virtual bool test_connection(wxString &error_msg) const = 0;

    // Upload a file to cloud storage
    virtual bool upload_file(const boost::filesystem::path &local_path,
                            const std::string &remote_path,
                            ProgressFn progress_fn,
                            ErrorFn error_fn) const = 0;

    // Download a file from cloud storage
    virtual bool download_file(const std::string &remote_path,
                               const boost::filesystem::path &local_path,
                               ProgressFn progress_fn,
                               ErrorFn error_fn) const = 0;

    // List files in a remote directory
    virtual bool list_files(const std::string &remote_path,
                           std::vector<CloudFile> &files,
                           ErrorFn error_fn) const = 0;

    // Delete a file from cloud storage
    virtual bool delete_file(const std::string &remote_path,
                            ErrorFn error_fn) const = 0;

    // Create a directory in cloud storage
    virtual bool create_directory(const std::string &remote_path,
                                 ErrorFn error_fn) const = 0;

    // Factory method to create appropriate provider based on config
    static std::unique_ptr<CloudSyncProvider> create_from_config(const AppConfig &config);

protected:
    std::string m_url;
    std::string m_username;
    std::string m_password;
};

// WebDAV implementation of CloudSyncProvider
class WebDAVCloudSync : public CloudSyncProvider
{
public:
    WebDAVCloudSync(const std::string &url, const std::string &username, const std::string &password);
    virtual ~WebDAVCloudSync() = default;

    const char* get_name() const override { return "WebDAV"; }

    bool test_connection(wxString &error_msg) const override;

    bool upload_file(const boost::filesystem::path &local_path,
                    const std::string &remote_path,
                    ProgressFn progress_fn,
                    ErrorFn error_fn) const override;

    bool download_file(const std::string &remote_path,
                      const boost::filesystem::path &local_path,
                      ProgressFn progress_fn,
                      ErrorFn error_fn) const override;

    bool list_files(const std::string &remote_path,
                   std::vector<CloudFile> &files,
                   ErrorFn error_fn) const override;

    bool delete_file(const std::string &remote_path,
                    ErrorFn error_fn) const override;

    bool create_directory(const std::string &remote_path,
                         ErrorFn error_fn) const override;

private:
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
