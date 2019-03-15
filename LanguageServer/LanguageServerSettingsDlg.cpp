#include "LanguageServerSettingsDlg.h"
#include "NewLanguageServerDlg.h"
#include "LanguageServerEntry.h"
#include "LanguageServerConfig.h"
#include "LanguageServerPage.h"
#include "globals.h"

LanguageServerSettingsDlg::LanguageServerSettingsDlg(wxWindow* parent)
    : LanguageServerSettingsDlgBase(parent)
{
    const LanguageServerEntry::Map_t& servers = LanguageServerConfig::Get().GetServers();
    for(const LanguageServerEntry::Map_t::value_type& vt : servers) {
        m_notebook->AddPage(new LanguageServerPage(m_notebook, vt.second), vt.second.GetName());
    }
    m_checkBoxEnable->SetValue(LanguageServerConfig::Get().IsEnabled());
    m_filePickerNodeJS->SetPath(LanguageServerConfig::Get().GetNodejs());
    ::clSetDialogBestSizeAndPosition(this);
}

LanguageServerSettingsDlg::~LanguageServerSettingsDlg() {}

void LanguageServerSettingsDlg::OnAddServer(wxCommandEvent& event)
{
    NewLanguageServerDlg dlg(this);
    if(dlg.ShowModal() == wxID_OK) {
        LanguageServerEntry server = dlg.GetData();
        // Update the configuration
        LanguageServerConfig::Get().AddServer(server);
        m_notebook->AddPage(new LanguageServerPage(m_notebook, server), server.GetName());
    }
}

void LanguageServerSettingsDlg::Save()
{
    LanguageServerConfig& conf = LanguageServerConfig::Get();
    for(size_t i=0; i<m_notebook->GetPageCount(); ++i) {
        LanguageServerPage* page = dynamic_cast<LanguageServerPage*>(m_notebook->GetPage(i));
        conf.AddServer(page->GetData());
    }
    conf.SetEnabled(GetCheckBoxEnable()->IsChecked());
    conf.SetNodejs(m_filePickerNodeJS->GetPath());
    conf.Save();
}