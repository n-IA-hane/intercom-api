import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID

from . import IntercomApi, CONF_INTERCOM_API_ID

DEPENDENCIES = ["intercom_api"]

intercom_api_ns = cg.esphome_ns.namespace("intercom_api")
IntercomApiStateSensor = intercom_api_ns.class_(
    "IntercomApiStateSensor", text_sensor.TextSensor, cg.Component
)

CONFIG_SCHEMA = text_sensor.text_sensor_schema(
    IntercomApiStateSensor,
    icon="mdi:phone-settings",
).extend(
    {
        cv.GenerateID(CONF_INTERCOM_API_ID): cv.use_id(IntercomApi),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_INTERCOM_API_ID])
    cg.add(var.set_parent(parent))
