"""Diagnostic text sensor listing panels that report on the bus but are not configured."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_TIGO_RS485_ID, TigoRS485

CONF_UNCONFIGURED_PANELS = "unconfigured_panels"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_TIGO_RS485_ID): cv.use_id(TigoRS485),
        cv.Optional(CONF_UNCONFIGURED_PANELS): text_sensor.text_sensor_schema(
            icon="mdi:solar-panel",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TIGO_RS485_ID])
    if CONF_UNCONFIGURED_PANELS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_UNCONFIGURED_PANELS])
        cg.add(parent.set_unconfigured_text_sensor(sens))
