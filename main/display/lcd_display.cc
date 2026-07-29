#include "lcd_display.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme (الوضع الفاتح - ناعم وعصري)
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xF8FAFC));        // خلفية بيضاء ناعمة
    light_theme->set_text_color(lv_color_hex(0x0F172A));              // نص كحلي غامق
    light_theme->set_chat_background_color(lv_color_hex(0xF1F5F9));   // خلفية منطقة الدردشة
    light_theme->set_user_bubble_color(lv_color_hex(0x2563EB));       // فقاعة الطالب (أزرق ملكي)
    light_theme->set_assistant_bubble_color(lv_color_hex(0xE2E8F0));  // فقاعة الروبوت (رمادي ناعم)
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x475569));
    light_theme->set_border_color(lv_color_hex(0xCBD5E1));
    light_theme->set_low_battery_color(lv_color_hex(0xEF4444));      // أحمر تحذيري
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme (الوضع الداكن - ممتاز لشاشة IPS)
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x0F172A));         // خلفية كحلية غامقة
    dark_theme->set_text_color(lv_color_hex(0xF8FAFC));               // نص أبيض ناصع
    dark_theme->set_chat_background_color(lv_color_hex(0x1E293B));    // خلفية المحادثة داكنة
    dark_theme->set_user_bubble_color(lv_color_hex(0x3B82F6));        // فقاعة الطالب (أزرق متوهج)
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x334155));   // فقاعة الروبوت (رمادي كحلي)
    dark_theme->set_system_bubble_color(lv_color_hex(0x1E293B));
    dark_theme->set_system_text_color(lv_color_hex(0x94A3B8));
    dark_theme->set_border_color(lv_color_hex(0x334155));
    dark_theme->set_low_battery_color(lv_color_hex(0xEF4444));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                  text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}
#endif
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
