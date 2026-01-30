import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODE, CONF_SAMPLE_RATE
from esphome.core import CORE
from esphome.components.esp32 import add_idf_component

CODEOWNERS = ["@n-IA-hane"]
DEPENDENCIES = ["esp32"]

# Only available on ESP32-S3 or ESP32-P4 with ESP-SR
_AEC_SUPPORTED_VARIANTS = ("ESP32S3", "ESP32P4")

def _validate_esp32_variant(config):
    if CORE.is_esp32:
        import esphome.components.esp32 as esp32
        variant = esp32.get_esp32_variant()
        if variant not in _AEC_SUPPORTED_VARIANTS:
            raise cv.Invalid(
                f"esp_aec requires {' or '.join(_AEC_SUPPORTED_VARIANTS)}, got {variant}"
            )
    return config

esp_aec_ns = cg.esphome_ns.namespace("esp_aec")
EspAec = esp_aec_ns.class_("EspAec", cg.Component)

CONF_FILTER_LENGTH = "filter_length"

AEC_MODES = {
    "sr_low_cost": 0,      # AEC_MODE_SR_LOW_COST
    "sr_high_perf": 1,     # AEC_MODE_SR_HIGH_PERF
    "voip_low_cost": 3,    # AEC_MODE_VOIP_LOW_COST
    "voip_high_perf": 4,   # AEC_MODE_VOIP_HIGH_PERF
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EspAec),
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=16000, max=16000),
            cv.Optional(CONF_FILTER_LENGTH, default=4): cv.int_range(min=1, max=8),
            cv.Optional(CONF_MODE, default="voip_low_cost"): cv.enum(AEC_MODES, lower=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_esp32_variant,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_filter_length(config[CONF_FILTER_LENGTH]))
    cg.add(var.set_mode(config[CONF_MODE]))

    # Add build flag to enable AEC code paths
    cg.add_define("USE_ESP_AEC")

    # Add ESP-SR as IDF component dependency (uses IDF component registry)
    add_idf_component(name="espressif/esp-sr", ref="2.3.0")
