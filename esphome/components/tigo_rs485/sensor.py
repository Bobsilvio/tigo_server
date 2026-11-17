"""Per-panel sensors. A panel is identified by its optimizer long address (stable)
or, as a fallback, by the short node ID assigned by the CCA (can change)."""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_CURRENT,
    CONF_POWER,
    CONF_TEMPERATURE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_PERCENT,
    UNIT_VOLT,
    UNIT_WATT,
)

from . import CONF_TIGO_RS485_ID, TigoRS485

CONF_LONG_ADDRESS = "long_address"
CONF_NODE_ID = "node_id"
CONF_VOLTAGE_IN = "voltage_in"
CONF_VOLTAGE_OUT = "voltage_out"
CONF_RSSI = "rssi"
CONF_DUTY_CYCLE = "duty_cycle"

_HEX = set("0123456789ABCDEF")


def validate_long_address(value):
    value = cv.string_strict(value).strip().upper().replace(":", "").replace("-", "")
    if len(value) != 16 or not set(value) <= _HEX or int(value, 16) == 0:
        raise cv.Invalid(
            "long_address must be 16 hex digits, e.g. 04C05B4000B1A688 "
            "(shown in the logs and in the 'unconfigured panels' text sensor)"
        )
    return value


# key -> (index in the C++ PanelSensor enum, schema)
SENSORS = {
    CONF_POWER: (
        0,
        sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_VOLTAGE_IN: (
        1,
        sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_VOLTAGE_OUT: (
        2,
        sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_CURRENT: (
        3,
        sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_TEMPERATURE: (
        4,
        sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_RSSI: (
        5,
        sensor.sensor_schema(
            icon="mdi:signal",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    CONF_DUTY_CYCLE: (
        6,
        sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            icon="mdi:sine-wave",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_TIGO_RS485_ID): cv.use_id(TigoRS485),
            cv.Optional(CONF_LONG_ADDRESS): validate_long_address,
            cv.Optional(CONF_NODE_ID): cv.hex_uint16_t,
            **{cv.Optional(key): schema for key, (_, schema) in SENSORS.items()},
        }
    ),
    cv.has_exactly_one_key(CONF_LONG_ADDRESS, CONF_NODE_ID),
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_TIGO_RS485_ID])
    if CONF_LONG_ADDRESS in config:
        long_address = cg.RawExpression(f"0x{config[CONF_LONG_ADDRESS]}ULL")
        node_id = -1
    else:
        long_address = cg.RawExpression("0ULL")
        node_id = config[CONF_NODE_ID]
    for key, (index, _) in SENSORS.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(parent.set_panel_sensor(long_address, node_id, index, sens))
