import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_SPEAKER,
    CONF_PORT,
    CONF_ICON,
    ENTITY_CATEGORY_CONFIG,
)
from esphome.components import microphone, speaker, switch

CODEOWNERS = ["@n-IA-hane"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["switch", "number"]

CONF_INTERCOM_API_ID = "intercom_api_id"
CONF_AUTO_ANSWER = "auto_answer"
CONF_DC_OFFSET_REMOVAL = "dc_offset_removal"
CONF_MIC_BITS = "mic_bits"
CONF_AEC_ID = "aec_id"
CONF_ON_RINGING = "on_ringing"
CONF_ON_CALL_END = "on_call_end"

intercom_api_ns = cg.esphome_ns.namespace("intercom_api")
IntercomApi = intercom_api_ns.class_("IntercomApi", cg.Component)
IntercomApiAutoAnswer = intercom_api_ns.class_(
    "IntercomApiAutoAnswer", switch.Switch, cg.Parented.template(IntercomApi)
)

def _aec_schema(value):
    """Validate aec_id - import esp_aec only if used."""
    if value is None:
        return value
    # Import here to avoid circular dependency
    from esphome.components import esp_aec
    return cv.use_id(esp_aec.EspAec)(value)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(IntercomApi),
        cv.Optional(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        # For 32-bit mics like SPH0645 that need conversion to 16-bit
        cv.Optional(CONF_MIC_BITS, default=16): cv.int_range(min=16, max=32),
        # DC offset removal for mics with significant DC bias (e.g., SPH0645)
        cv.Optional(CONF_DC_OFFSET_REMOVAL, default=False): cv.boolean,
        # Optional AEC (Acoustic Echo Cancellation) component
        cv.Optional(CONF_AEC_ID): _aec_schema,
        # Trigger when incoming call (auto_answer OFF)
        cv.Optional(CONF_ON_RINGING): automation.validate_automation(single=True),
        # Trigger when call ends (hangup, decline, or answered)
        cv.Optional(CONF_ON_CALL_END): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_MICROPHONE in config:
        mic = await cg.get_variable(config[CONF_MICROPHONE])
        cg.add(var.set_microphone(mic))

    if CONF_SPEAKER in config:
        spk = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(spk))

    cg.add(var.set_mic_bits(config[CONF_MIC_BITS]))
    cg.add(var.set_dc_offset_removal(config[CONF_DC_OFFSET_REMOVAL]))

    if CONF_AEC_ID in config and config[CONF_AEC_ID] is not None:
        aec = await cg.get_variable(config[CONF_AEC_ID])
        cg.add(var.set_aec(aec))

    # on_ringing automation
    if CONF_ON_RINGING in config:
        await automation.build_automation(
            var.get_ringing_trigger(), [], config[CONF_ON_RINGING]
        )

    # on_call_end automation
    if CONF_ON_CALL_END in config:
        await automation.build_automation(
            var.get_call_end_trigger(), [], config[CONF_ON_CALL_END]
        )

    # NOTE: Auto-answer switch should be defined manually in YAML for proper restore behavior
