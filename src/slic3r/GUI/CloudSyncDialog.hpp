///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_CloudSyncDialog_hpp_
#define slic3r_CloudSyncDialog_hpp_

#include "MsgDialog.hpp"
#include <wx/radiobut.h>

namespace Slic3r {
namespace GUI {

// Dialog for choosing the direction of first cloud sync
class FirstSyncDirectionDialog : public MsgDialog
{
public:
    enum SyncDirection {
        DIRECTION_NONE,
        DIRECTION_DOWNLOAD,  // Download from cloud (replaces local)
        DIRECTION_UPLOAD     // Upload to cloud (replaces cloud)
    };

    FirstSyncDirectionDialog(
        wxWindow *parent,
        size_t local_file_count,
        size_t remote_file_count,
        time_t local_last_modified,
        time_t remote_last_modified
    );

    FirstSyncDirectionDialog(FirstSyncDirectionDialog &&) = delete;
    FirstSyncDirectionDialog(const FirstSyncDirectionDialog &) = delete;
    FirstSyncDirectionDialog &operator=(FirstSyncDirectionDialog &&) = delete;
    FirstSyncDirectionDialog &operator=(const FirstSyncDirectionDialog &) = delete;
    virtual ~FirstSyncDirectionDialog() = default;

    // Get the user's choice after ShowModal()
    SyncDirection get_direction() const { return m_direction; }

private:
    wxRadioButton *m_radio_download;
    wxRadioButton *m_radio_upload;
    SyncDirection m_direction;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_CloudSyncDialog_hpp_
