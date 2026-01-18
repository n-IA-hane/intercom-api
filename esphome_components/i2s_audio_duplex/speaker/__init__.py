"""I2S Audio Duplex Speaker Platform - Wraps duplex bus as standard ESPHome speaker"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import speaker
from esphome.const import CONF_ID
from .. import (
    i2s_audio_duplex_ns,
    I2SAudioDuplex,
    CONF_I2S_AUDIO_DUPLEX_ID,
)

DEPENDENCIES = ["i2s_audio_duplex"]
CODEOWNERS = ["@n-IA-hane"]

I2SAudioDuplexSpeaker = i2s_audio_duplex_ns.class_(
    "I2SAudioDuplexSpeaker",
    speaker.Speaker,
    cg.Component,
    cg.Parented.template(I2SAudioDuplex),
)

CONFIG_SCHEMA = speaker.SPEAKER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(I2SAudioDuplexSpeaker),
        cv.GenerateID(CONF_I2S_AUDIO_DUPLEX_ID): cv.use_id(I2SAudioDuplex),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    parent = await cg.get_variable(config[CONF_I2S_AUDIO_DUPLEX_ID])
    cg.add(var.set_parent(parent))
