"""Per-panel sensors. A panel is identified by its optimizer long address (stable)
or, as a fallback, by the short node ID assigned by the CCA (can change)."""

import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv

from esphome.const import CONF_NAME

from . import CONF_TIGO_RS485_ID, SENSOR_NAMES, SENSORS, TigoRS485

CONF_LONG_ADDRESS = "long_address"
CONF_NODE_ID = "node_id"

_HEX = set("0123456789ABCDEF")


def validate_long_address(value):
    value = cv.string_strict(value).strip().upper().replace(":", "").replace("-", "")
    if len(value) != 16 or not set(value) <= _HEX or int(value, 16) == 0:
        raise cv.Invalid(
            "long_address must be 16 hex digits, e.g. 04C05B4000B1A688 "
            "(shown in the logs and in the 'unconfigured panels' text sensor)"
        )
    return value


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
    cg.add(parent.set_panel_label(long_address, node_id, _panel_label(config)))


def _panel_label(config):
    """Name shown on the web dashboard: the sensor names without their suffix
    ("A1 Power" -> "A1"), else the address."""
    for key in SENSORS:
        name = config.get(key, {}).get(CONF_NAME)
        if not name:
            continue
        suffix = " " + SENSOR_NAMES[key]
        if name.lower().endswith(suffix.lower()) and len(name) > len(suffix):
            return name[: -len(suffix)]
        return name
    if CONF_LONG_ADDRESS in config:
        return config[CONF_LONG_ADDRESS]
    return f"node {config[CONF_NODE_ID]:04X}"
