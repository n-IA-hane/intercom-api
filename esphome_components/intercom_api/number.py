import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import (
    CONF_ID,
    CONF_MIN_VALUE,
    CONF_MAX_VALUE,
    CONF_STEP,
    CONF_MODE,
    CONF_ICON,
)

from . import intercom_api_ns, IntercomApi, CONF_INTERCOM_API_ID

DEPENDENCIES = ["intercom_api"]

IntercomApiVolume = intercom_api_ns.class_(
    "IntercomApiVolume", number.Number, cg.Parented.template(IntercomApi)
)

CONFIG_SCHEMA = number.number_schema(
    IntercomApiVolume,
    icon="mdi:volume-high",
).extend(
    {
        cv.GenerateID(CONF_INTERCOM_API_ID): cv.use_id(IntercomApi),
        cv.Optional(CONF_MIN_VALUE, default=0): cv.float_,
        cv.Optional(CONF_MAX_VALUE, default=100): cv.float_,
        cv.Optional(CONF_STEP, default=1): cv.float_,
        cv.Optional(CONF_MODE, default="SLIDER"): cv.enum(
            number.NUMBER_MODES, upper=True
        ),
    }
)


async def to_code(config):
    var = await number.new_number(
        config,
        min_value=config[CONF_MIN_VALUE],
        max_value=config[CONF_MAX_VALUE],
        step=config[CONF_STEP],
    )
    parent = await cg.get_variable(config[CONF_INTERCOM_API_ID])
    cg.add(var.set_parent(parent))
