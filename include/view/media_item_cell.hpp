/**
 * VitaPlex - Media Item Cell
 * A cell for displaying media items in a grid
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"

namespace vitaplex {

class MediaItemCell : public brls::Box {
public:
    MediaItemCell();

    void setItem(const MediaItem& item);
    const MediaItem& getItem() const { return m_item; }

    static brls::View* create();

private:
    void loadThumbnail();

    MediaItem m_item;

    brls::Image* m_thumbnailImage = nullptr;
    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_subtitleLabel = nullptr;
    brls::Rectangle* m_progressBar = nullptr;
};

} // namespace vitaplex
