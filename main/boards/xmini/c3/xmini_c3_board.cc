#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "mcp_server.h"
#include "settings.h"
#include "config.h"
#include "power_save_timer.h"
#include "press_to_talk_mcp_tool.h"

#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <cJSON.h>
#include <esp_log.h>
#include <string>

#define TAG "XminiC3Board"
namespace {
 
const char* kWeatherToolTag = "WeatherTool";
 
// Gửi GET request và trả về toàn bộ nội dung response dạng string
esp_err_t HttpGetToString(const char* url, std::string& out) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 8000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
 
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(kWeatherToolTag, "Khong ket noi duoc: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
 
    char buf[512];
    int read_len;
    out.clear();
    while ((read_len = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        out.append(buf, read_len);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}
 
// Chuyển mã thời tiết WMO của Open-Meteo thành mô tả ngắn tiếng Việt
std::string WeatherCodeToText(int code) {
    if (code == 0) return "troi quang";
    if (code <= 3) return "co may";
    if (code <= 48) return "suong mu";
    if (code <= 57) return "mua phun";
    if (code <= 67) return "mua";
    if (code <= 77) return "tuyet";
    if (code <= 82) return "mua rao";
    if (code <= 86) return "mua tuyet";
    if (code <= 99) return "giong bao";
    return "khong ro";
}
 
// Lấy vị trí hiện tại theo IP + thời tiết từ Open-Meteo, trả về 1 câu mô tả
std::string GetWeatherReport() {
    // 1. Xac dinh vi tri qua IP cong cong (khong can GPS/toa do)
    std::string geo;
    if (HttpGetToString("http://ip-api.com/json/?fields=lat,lon,city", geo) != ESP_OK) {
        return "Khong lay duoc vi tri thiet bi, kiem tra ket noi mang.";
    }
    cJSON* geo_json = cJSON_Parse(geo.c_str());
    if (geo_json == nullptr) {
        return "Loi doc du lieu vi tri.";
    }
    cJSON* lat_item = cJSON_GetObjectItem(geo_json, "lat");
    cJSON* lon_item = cJSON_GetObjectItem(geo_json, "lon");
    cJSON* city_item = cJSON_GetObjectItem(geo_json, "city");
    if (lat_item == nullptr || lon_item == nullptr) {
        cJSON_Delete(geo_json);
        return "Khong doc duoc toa do vi tri.";
    }
    double lat = lat_item->valuedouble;
    double lon = lon_item->valuedouble;
    std::string city = (city_item != nullptr && city_item->valuestring != nullptr)
                            ? city_item->valuestring : "khu vuc cua ban";
    cJSON_Delete(geo_json);
 
    // 2. Goi Open-Meteo lay thoi tiet hien tai theo toa do vua tim duoc
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code",
             lat, lon);
 
    std::string weather;
    if (HttpGetToString(url, weather) != ESP_OK) {
        return "Khong lay duoc du lieu thoi tiet.";
    }
    cJSON* w_json = cJSON_Parse(weather.c_str());
    if (w_json == nullptr) {
        return "Loi doc du lieu thoi tiet.";
    }
    cJSON* current = cJSON_GetObjectItem(w_json, "current");
    if (current == nullptr) {
        cJSON_Delete(w_json);
        return "Du lieu thoi tiet khong hop le.";
    }
    double temp = cJSON_GetObjectItem(current, "temperature_2m")->valuedouble;
    int humidity = cJSON_GetObjectItem(current, "relative_humidity_2m")->valueint;
    int code = cJSON_GetObjectItem(current, "weather_code")->valueint;
    cJSON_Delete(w_json);
 
    char result[192];
    snprintf(result, sizeof(result), "%s: %s, %.1f do C, do am %d%%",
             city.c_str(), WeatherCodeToText(code).c_str(), temp, humidity);
    return std::string(result);
}
 
}  // namespace

class XminiC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    PowerSaveTimer* power_save_timer_ = nullptr;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(160, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // This board burns ESP_EFUSE_VDD_SPI_AS_GPIO which permanently damages
        // incompatible boards, so we must be certain the ES8311 codec is really
        // present before continuing. i2c_master_probe() only checks for an ACK,
        // which can be a false positive on a wrong board (floating / weakly
        // pulled SDA). Instead, verify the ES8311 chip ID registers.
        if (!IsEs8311Present()) {
            while (true) {
                ESP_LOGE(TAG, "ES8311 not detected, please check if you have installed the correct firmware");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        }
    }

    // Read the ES8311 chip ID registers (0xFD/0xFE should return 0x83/0x11).
    bool IsEs8311Present() {
        i2c_master_dev_handle_t dev = nullptr;
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x18,
            .scl_speed_hz = 100 * 1000,
        };
        if (i2c_master_bus_add_device(codec_i2c_bus_, &dev_cfg, &dev) != ESP_OK) {
            return false;
        }

        uint8_t reg = 0xFD;
        uint8_t id1 = 0, id2 = 0;
        esp_err_t err1 = i2c_master_transmit_receive(dev, &reg, 1, &id1, 1, 100);
        reg = 0xFE;
        esp_err_t err2 = i2c_master_transmit_receive(dev, &reg, 1, &id2, 1, 100);
        i2c_master_bus_rm_device(dev);

        ESP_LOGI(TAG, "ES8311 chip id: err=(%s,%s) id=0x%02X 0x%02X",
            esp_err_to_name(err1), esp_err_to_name(err2), id1, id2);
        return err1 == ESP_OK && err2 == ESP_OK && id1 == 0x83 && id2 == 0x11;
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        // IDF 5.5 and 6.0 declare these fields in a different order. Assign
        // them individually so C++ designated-initializer ordering is irrelevant.
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400 * 1000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.on_color_trans_done = nullptr;
        io_config.user_ctx = nullptr;
        io_config.flags.dc_low_on_data = 0;
        io_config.flags.disable_control_phase = 0;

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(codec_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressDown([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });
    }

    void InitializeTools() {
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool(
        "self.get_weather",
        "Lay thoi tiet hien tai theo vi tri IP cua thiet bi va hien len man hinh.",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            std::string report = GetWeatherReport();
            // Hien thi len man OLED - neu dong nay bao loi, kiem tra ten ham
            // dung trong main/display/display.h cua repo (co the la SetStatus,
            // ShowNotification, SetChatMessage... tuy phien ban)
            if (GetDisplay() != nullptr) {
                GetDisplay()->SetChatMessage("system", report.c_str());
            }
            return report;  // AI se doc cau nay bang giong noi
        });
    }

public:
    XminiC3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeTools();

        // 避免使用错误的固件，把 EFUSE 操作放在最后
        // 把 ESP32C3 的 VDD SPI 引脚作为普通 GPIO 口使用
        esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(XminiC3Board);
