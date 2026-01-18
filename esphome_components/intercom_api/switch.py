import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, CONF_NAME, CONF_ICON

from . import intercom_api_ns, IntercomApi, CONF_INTERCOM_API_ID

DEPENDENCIES = ["intercom_api"]

IntercomApiSwitch = intercom_api_ns.class_(
    "IntercomApiSwitch", switch.Switch, cg.Parented.template(IntercomApi)
)

CONFIG_SCHEMA = switch.switch_schema(
    IntercomApiSwitch,
    icon="mdi:phone",
).extend(
    {
        cv.GenerateID(CONF_INTERCOM_API_ID): cv.use_id(IntercomApi),
    }
)


async def to_code(config):
    var = await switch.new_switch(config)
    parent = await cg.get_variable(config[CONF_INTERCOM_API_ID])
    cg.add(var.set_parent(parent))
