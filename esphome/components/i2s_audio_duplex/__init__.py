"""I2S Audio Duplex Component - Full duplex I2S for simultaneous mic+speaker

Exposes standard ESPHome microphone and speaker platforms for compatibility with
Voice Assistant and intercom_api components.

Multi-rate support: set output_sample_rate to decimate mic audio internally.
  sample_rate: I2S bus rate (e.g. 48000 for high-quality DAC output)
  output_sample_rate: mic/AEC/MWW/VA rate (e.g. 16000, must divide sample_rate evenly)
If output_sample_rate is omitted, no decimation occurs (backward compatible).
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_SAMPLE_RATE

CODEOWNERS = ["@n-IA-hane"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["switch", "number"]

CONF_I2S_LRCLK_PIN = "i2s_lrclk_pin"
CONF_I2S_BCLK_PIN = "i2s_bclk_pin"
CONF_I2S_MCLK_PIN = "i2s_mclk_pin"
CONF_I2S_DIN_PIN = "i2s_din_pin"
CONF_I2S_DOUT_PIN = "i2s_dout_pin"
CONF_OUTPUT_SAMPLE_RATE = "output_sample_rate"
CONF_AEC_ID = "aec_id"
CONF_AEC_REF_DELAY_MS = "aec_reference_delay_ms"
CONF_MIC_ATTENUATION = "mic_attenuation"
CONF_USE_STEREO_AEC_REF = "use_stereo_aec_reference"
CONF_REFERENCE_CHANNEL = "reference_channel"
CONF_USE_TDM_REFERENCE = "use_tdm_reference"
CONF_TDM_TOTAL_SLOTS = "tdm_total_slots"
CONF_TDM_MIC_SLOT = "tdm_mic_slot"
CONF_TDM_REF_SLOT = "tdm_ref_slot"
CONF_I2S_AUDIO_DUPLEX_ID = "i2s_audio_duplex_id"

i2s_audio_duplex_ns = cg.esphome_ns.namespace("i2s_audio_duplex")
I2SAudioDuplex = i2s_audio_duplex_ns.class_("I2SAudioDuplex", cg.Component)

# Forward declare esp_aec
esp_aec_ns = cg.esphome_ns.namespace("esp_aec")
EspAec = esp_aec_ns.class_("EspAec")


def _validate_sample_rates(config):
    """Validate that output_sample_rate divides sample_rate evenly."""
    if CONF_OUTPUT_SAMPLE_RATE in config:
        sr = config[CONF_SAMPLE_RATE]
        osr = config[CONF_OUTPUT_SAMPLE_RATE]
        if sr % osr != 0:
            raise cv.Invalid(
                f"sample_rate ({sr}) must be an exact multiple of "
                f"output_sample_rate ({osr})"
            )
        ratio = sr // osr
        if ratio > 6:
            raise cv.Invalid(
                f"Decimation ratio {ratio} (={sr}/{osr}) exceeds maximum of 6"
            )
    return config


def _validate_tdm_config(config):
    """Validate TDM reference configuration."""
    use_tdm = config.get(CONF_USE_TDM_REFERENCE, False)
    use_stereo = config.get(CONF_USE_STEREO_AEC_REF, False)

    if use_tdm and use_stereo:
        raise cv.Invalid(
            "use_tdm_reference and use_stereo_aec_reference are mutually exclusive"
        )

    if use_tdm:
        total_slots = config.get(CONF_TDM_TOTAL_SLOTS, 4)
        mic_slot = config.get(CONF_TDM_MIC_SLOT, 0)
        ref_slot = config.get(CONF_TDM_REF_SLOT, 1)

        if mic_slot == ref_slot:
            raise cv.Invalid(
                f"tdm_mic_slot ({mic_slot}) and tdm_ref_slot ({ref_slot}) must differ"
            )

        max_slot = max(mic_slot, ref_slot)
        if total_slots <= max_slot:
            raise cv.Invalid(
                f"tdm_total_slots ({total_slots}) must be > {max_slot} "
                f"(highest slot index)"
            )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(I2SAudioDuplex),
        cv.Required(CONF_I2S_LRCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_I2S_BCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_I2S_MCLK_PIN, default=-1): cv.Any(
            cv.int_range(min=-1, max=-1),
            pins.internal_gpio_output_pin_number,
        ),
        cv.Optional(CONF_I2S_DIN_PIN, default=-1): cv.Any(
            cv.int_range(min=-1, max=-1),
            pins.internal_gpio_input_pin_number,
        ),
        cv.Optional(CONF_I2S_DOUT_PIN, default=-1): cv.Any(
            cv.int_range(min=-1, max=-1),
            pins.internal_gpio_output_pin_number,
        ),
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=8000, max=48000),
        # Output sample rate for mic/AEC/MWW/VA (decimated from bus rate)
        # If omitted, equals sample_rate (no decimation)
        cv.Optional(CONF_OUTPUT_SAMPLE_RATE): cv.int_range(min=8000, max=48000),
        cv.Optional(CONF_AEC_ID): cv.use_id(EspAec),
        # AEC reference delay: 80ms for separate I2S, 20-40ms for integrated codecs like ES8311
        cv.Optional(CONF_AEC_REF_DELAY_MS, default=80): cv.int_range(min=10, max=200),
        # Pre-AEC mic attenuation: 0.1 = -20dB (for hot mics like ES8311 that overdrive)
        cv.Optional(CONF_MIC_ATTENUATION, default=1.0): cv.float_range(min=0.01, max=1.0),
        # ES8311 digital feedback: RX is stereo with L=DAC(reference), R=ADC(mic)
        # Requires ES8311 register 0x44 bits[6:4]=4 (ADCDAT_SEL=DACL+ADC)
        cv.Optional(CONF_USE_STEREO_AEC_REF, default=False): cv.boolean,
        # Which stereo channel carries the AEC reference (default: left)
        cv.Optional(CONF_REFERENCE_CHANNEL, default="left"): cv.one_of("left", "right", lower=True),
        # TDM hardware reference: ES7210 in TDM mode with one slot carrying DAC feedback
        cv.Optional(CONF_USE_TDM_REFERENCE, default=False): cv.boolean,
        cv.Optional(CONF_TDM_TOTAL_SLOTS, default=4): cv.int_range(min=2, max=8),
        cv.Optional(CONF_TDM_MIC_SLOT, default=0): cv.int_range(min=0, max=7),
        cv.Optional(CONF_TDM_REF_SLOT, default=1): cv.int_range(min=0, max=7),
    }).extend(cv.COMPONENT_SCHEMA),
    _validate_sample_rates,
    _validate_tdm_config,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Define USE_I2S_AUDIO_DUPLEX so other components know it's available
    cg.add_define("USE_I2S_AUDIO_DUPLEX")

    cg.add(var.set_lrclk_pin(config[CONF_I2S_LRCLK_PIN]))
    cg.add(var.set_bclk_pin(config[CONF_I2S_BCLK_PIN]))
    cg.add(var.set_mclk_pin(config[CONF_I2S_MCLK_PIN]))
    cg.add(var.set_din_pin(config[CONF_I2S_DIN_PIN]))
    cg.add(var.set_dout_pin(config[CONF_I2S_DOUT_PIN]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))

    # Set output sample rate if specified (enables decimation)
    if CONF_OUTPUT_SAMPLE_RATE in config:
        cg.add(var.set_output_sample_rate(config[CONF_OUTPUT_SAMPLE_RATE]))

    # Set AEC reference delay (must be set BEFORE set_aec for buffer sizing)
    cg.add(var.set_aec_reference_delay_ms(config[CONF_AEC_REF_DELAY_MS]))

    # Set mic attenuation for hot mics (applied BEFORE AEC)
    cg.add(var.set_mic_attenuation(config[CONF_MIC_ATTENUATION]))

    # ES8311 digital feedback mode: stereo RX with L=ref, R=mic
    cg.add(var.set_use_stereo_aec_reference(config[CONF_USE_STEREO_AEC_REF]))

    # Reference channel selection (left or right)
    cg.add(var.set_reference_channel_right(config[CONF_REFERENCE_CHANNEL] == "right"))

    # TDM hardware reference configuration
    if config[CONF_USE_TDM_REFERENCE]:
        cg.add(var.set_use_tdm_reference(True))
        cg.add(var.set_tdm_total_slots(config[CONF_TDM_TOTAL_SLOTS]))
        cg.add(var.set_tdm_mic_slot(config[CONF_TDM_MIC_SLOT]))
        cg.add(var.set_tdm_ref_slot(config[CONF_TDM_REF_SLOT]))

    # Link AEC if configured
    if CONF_AEC_ID in config:
        aec = await cg.get_variable(config[CONF_AEC_ID])
        cg.add(var.set_aec(aec))
        # Enable AEC compilation in i2s_audio_duplex
        cg.add_define("USE_ESP_AEC")
