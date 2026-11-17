"""Tigo RS485 Monitor: passive listener on the Tigo TAP <-> CCA RS-485 bus."""

import gzip
from pathlib import Path

import esphome.codegen as cg
# Aliased: the platform modules of this package (sensor.py, text_sensor.py) would
# otherwise replace these names in the package namespace once they are loaded
from esphome.components import button as button_, sensor as sensor_, text_sensor as text_sensor_, uart
import esphome.config_validation as cv
from esphome.core import CORE
from esphome.const import (
    CONF_CURRENT,
    CONF_ID,
    CONF_NAME,
    CONF_POWER,
    CONF_TEMPERATURE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_PERCENT,
    UNIT_VOLT,
    UNIT_WATT,
)

CODEOWNERS = ["@Bobsilvio"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor", "button"]
MULTI_CONF = True

CONF_TIGO_RS485_ID = "tigo_rs485_id"
CONF_OFFLINE_TIMEOUT = "offline_timeout"
CONF_PANELS = "panels"
CONF_PANEL_SENSORS = "panel_sensors"
CONF_PANEL_NAME = "panel_name"
CONF_VOLTAGE_IN = "voltage_in"
CONF_VOLTAGE_OUT = "voltage_out"
CONF_RSSI = "rssi"
CONF_DUTY_CYCLE = "duty_cycle"
# Entities generated for the automatic panel slots (filled in during validation)
CONF_SLOTS = "slots"
CONF_SLOT_ADDRESS = "address"
CONF_RESET = "reset_button"
CONF_LABEL = "label"
CONF_WEB_DASHBOARD = "web_dashboard"

DASHBOARD_FILE = Path(__file__).parent / "dashboard" / "index.html"

MAX_PANELS = 64

tigo_rs485_ns = cg.esphome_ns.namespace("tigo_rs485")
TigoRS485 = tigo_rs485_ns.class_("TigoRS485", cg.Component, uart.UARTDevice)
ResetSlotsButton = tigo_rs485_ns.class_("ResetSlotsButton", button_.Button)

# key -> (index in the C++ PanelSensor enum, schema)
SENSORS = {
    CONF_POWER: (
        0,
        sensor_.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_VOLTAGE_IN: (
        1,
        sensor_.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_VOLTAGE_OUT: (
        2,
        sensor_.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_CURRENT: (
        3,
        sensor_.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_TEMPERATURE: (
        4,
        sensor_.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    ),
    CONF_RSSI: (
        5,
        sensor_.sensor_schema(
            icon="mdi:signal",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    CONF_DUTY_CYCLE: (
        6,
        sensor_.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            icon="mdi:sine-wave",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
}

# Entity name suffix of each sensor in the automatic slots
SENSOR_NAMES = {
    CONF_POWER: "Power",
    CONF_VOLTAGE_IN: "Voltage In",
    CONF_VOLTAGE_OUT: "Voltage Out",
    CONF_CURRENT: "Current",
    CONF_TEMPERATURE: "Temperature",
    CONF_RSSI: "RSSI",
    CONF_DUTY_CYCLE: "Duty Cycle",
}

ADDRESS_SCHEMA = text_sensor_.text_sensor_schema(
    icon="mdi:identifier", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
)
RESET_SCHEMA = button_.button_schema(
    ResetSlotsButton, icon="mdi:restart", entity_category=ENTITY_CATEGORY_CONFIG
)


def _validate_panel_name(value):
    value = cv.string_strict(value)
    if "{}" not in value:
        raise cv.Invalid("panel_name must contain {} where the panel number goes, e.g. 'Panel {}'")
    return value


def _expand_slots(config):
    """Generate the entities of the automatic panel slots: one set of sensors per slot,
    an address text sensor per slot and the reset button."""
    count = config[CONF_PANELS]
    slots = []
    for i in range(1, count + 1):
        base = config[CONF_PANEL_NAME].format(i)
        slot = {
            key: SENSORS[key][1]({CONF_NAME: f"{base} {SENSOR_NAMES[key]}"})
            for key in config[CONF_PANEL_SENSORS]
        }
        slot[CONF_SLOT_ADDRESS] = ADDRESS_SCHEMA({CONF_NAME: f"{base} Address"})
        slot[CONF_LABEL] = base
        slots.append(slot)
    config[CONF_SLOTS] = slots
    if count:
        config[CONF_RESET] = RESET_SCHEMA({CONF_NAME: "Reset panel assignments"})
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TigoRS485),
            cv.Optional(
                CONF_OFFLINE_TIMEOUT, default="5min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_PANELS, default=0): cv.int_range(min=0, max=MAX_PANELS),
            cv.Optional(CONF_PANEL_SENSORS, default=list(SENSORS)): cv.All(
                cv.ensure_list(cv.one_of(*SENSORS, lower=True)), cv.Length(min=1)
            ),
            cv.Optional(CONF_PANEL_NAME, default="Panel {}"): _validate_panel_name,
            cv.Optional(CONF_WEB_DASHBOARD, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    _expand_slots,
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "tigo_rs485", baud_rate=38400, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_offline_timeout(config[CONF_OFFLINE_TIMEOUT]))
    for slot_index, slot in enumerate(config[CONF_SLOTS]):
        for key, (index, _) in SENSORS.items():
            if key in slot:
                sens = await sensor_.new_sensor(slot[key])
                cg.add(var.set_slot_sensor(slot_index, index, sens))
        addr = await text_sensor_.new_text_sensor(slot[CONF_SLOT_ADDRESS])
        cg.add(var.set_slot_address_sensor(slot_index, addr))
        cg.add(var.set_slot_label(slot_index, slot[CONF_LABEL]))
    if CONF_RESET in config:
        btn = await button_.new_button(config[CONF_RESET])
        await cg.register_parented(btn, var)

    # Web dashboard: served at "/" when the device also has web_server
    if config[CONF_WEB_DASHBOARD] and "web_server" in CORE.config:
        data = gzip.compress(DASHBOARD_FILE.read_bytes(), mtime=0)
        name = f"TIGO_DASHBOARD_{config[CONF_ID].id.upper()}"
        cg.add_global(
            cg.RawExpression(
                f"static const uint8_t {name}[{len(data)}] PROGMEM = {{{', '.join(map(str, data))}}}"
            )
        )
        cg.add(var.set_dashboard(cg.RawExpression(name), len(data)))
