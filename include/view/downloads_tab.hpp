/**
 * VitaPlex - Downloads Tab
 * View for managing offline downloads
 */

#pragma once

#include <borealis.hpp>

namespace vitaplex {

class DownloadsTab : public brls::Box {
public:
    DownloadsTab();
    ~DownloadsTab() override = default;

    void willAppear(bool resetState) override;

private:
    void refresh();
    void showDownloadOptions(const std::string& ratingKey, const std::string& title);

    brls::Box* m_listContainer = nullptr;
    brls::Label* m_emptyLabel = nullptr;
};

} // namespace vitaplex
