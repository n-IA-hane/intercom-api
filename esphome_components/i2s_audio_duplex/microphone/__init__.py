"""I2S Audio Duplex Microphone Platform - Wraps duplex bus as standard ESPHome microphone"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import audio, microphone
from esphome.const import CONF_ID
from .. import (
    i2s_audio_duplex_ns,
    I2SAudioDuplex,
    CONF_I2S_AUDIO_DUPLEX_ID,
)

DEPENDENCIES = ["i2s_audio_duplex"]
CODEOWNERS = ["@n-IA-hane"]

I2SAudioDuplexMicrophone = i2s_audio_duplex_ns.class_(
    "I2SAudioDuplexMicrophone",
    microphone.Microphone,
    cg.Component,
    cg.Parented.template(I2SAudioDuplex),
)


def _set_stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=1,
        min_sample_rate=16000,
        max_sample_rate=16000,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(I2SAudioDuplexMicrophone),
            cv.GenerateID(CONF_I2S_AUDIO_DUPLEX_ID): cv.use_id(I2SAudioDuplex),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)

    parent = await cg.get_variable(config[CONF_I2S_AUDIO_DUPLEX_ID])
    cg.add(var.set_parent(parent))
