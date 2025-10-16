///|/ Copyright (c) Prusa Research 2025
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "CloudSyncDialog.hpp"
#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/radiobut.h>
#include <wx/button.h>

namespace Slic3r {
namespace GUI {

FirstSyncDirectionDialog::FirstSyncDirectionDialog(
    wxWindow *parent,
    size_t local_file_count,
    size_t remote_file_count,
    time_t local_last_modified,
    time_t remote_last_modified
)
    : MsgDialog(parent, _L("Cloud Sync Setup"), _L("First Sync Configuration"), wxCAPTION | wxCLOSE_BOX)
    , m_direction(DIRECTION_NONE)
{
    // Format timestamps
    wxString local_date = _L("Unknown");
    wxString remote_date = _L("Unknown");

    if (local_last_modified > 0) {
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&local_last_modified));
        local_date = wxString::FromUTF8(buf);
    }

    if (remote_last_modified > 0) {
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&remote_last_modified));
        remote_date = wxString::FromUTF8(buf);
    }

    // Main message
    auto *text = new wxStaticText(this, wxID_ANY,
        _L("Both local and cloud storage contain configuration data.\n"
           "Please choose which direction to sync for the first time.\n\n"
           "A backup of the overwritten data will be created automatically."));
    text->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
    content_sizer->Add(text, 0, wxEXPAND | wxALL, BORDER);

    // Local info
    auto *local_label = new wxStaticText(this, wxID_ANY, _L("Local:"));
    local_label->SetFont(boldfont);
    content_sizer->Add(local_label, 0, wxLEFT | wxRIGHT, BORDER);

    wxString local_info = wxString::Format(
        _L("  %zu configuration files\n  Last modified: %s"),
        local_file_count,
        local_date
    );
    auto *local_text = new wxStaticText(this, wxID_ANY, local_info);
    content_sizer->Add(local_text, 0, wxLEFT | wxRIGHT | wxBOTTOM, BORDER);

    // Remote info
    auto *remote_label = new wxStaticText(this, wxID_ANY, _L("Cloud:"));
    remote_label->SetFont(boldfont);
    content_sizer->Add(remote_label, 0, wxLEFT | wxRIGHT, BORDER);

    wxString remote_info = wxString::Format(
        _L("  %zu configuration files\n  Last modified: %s"),
        remote_file_count,
        remote_date
    );
    auto *remote_text = new wxStaticText(this, wxID_ANY, remote_info);
    content_sizer->Add(remote_text, 0, wxLEFT | wxRIGHT | wxBOTTOM, BORDER);

    // Add spacing
    content_sizer->AddSpacer(VERT_SPACING);

    // Direction choices
    auto *choice_label = new wxStaticText(this, wxID_ANY, _L("Choose sync direction:"));
    choice_label->SetFont(boldfont);
    content_sizer->Add(choice_label, 0, wxLEFT | wxRIGHT, BORDER);

    m_radio_download = new wxRadioButton(this, wxID_ANY,
        _L("Download from cloud (replace local with cloud data)"),
        wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    content_sizer->Add(m_radio_download, 0, wxLEFT | wxRIGHT | wxTOP, BORDER);

    m_radio_upload = new wxRadioButton(this, wxID_ANY,
        _L("Upload to cloud (replace cloud with local data)"));
    content_sizer->Add(m_radio_upload, 0, wxLEFT | wxRIGHT | wxBOTTOM, BORDER);

    // Suggest the newer data as default
    if (local_last_modified > remote_last_modified) {
        m_radio_upload->SetValue(true);
    } else {
        m_radio_download->SetValue(true);
    }

    // Buttons
    auto *btn_ok = add_button(wxID_OK, true);
    btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_radio_download->GetValue()) {
            m_direction = DIRECTION_DOWNLOAD;
        } else if (m_radio_upload->GetValue()) {
            m_direction = DIRECTION_UPLOAD;
        }
        EndModal(wxID_OK);
    });

    auto *btn_cancel = add_button(wxID_CANCEL);
    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_direction = DIRECTION_NONE;
        EndModal(wxID_CANCEL);
    });

    finalize();
}

} // namespace GUI
} // namespace Slic3r
