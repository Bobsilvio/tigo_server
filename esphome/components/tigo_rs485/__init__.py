"""Tigo RS485 Monitor: passive listener on the Tigo TAP <-> CCA RS-485 bus."""

import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@Bobsilvio"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor"]
MULTI_CONF = True

CONF_TIGO_RS485_ID = "tigo_rs485_id"
CONF_OFFLINE_TIMEOUT = "offline_timeout"

tigo_rs485_ns = cg.esphome_ns.namespace("tigo_rs485")
TigoRS485 = tigo_rs485_ns.class_("TigoRS485", cg.Component, uart.UARTDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TigoRS485),
            cv.Optional(
                CONF_OFFLINE_TIMEOUT, default="5min"
            ): cv.positive_time_period_milliseconds,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "tigo_rs485", baud_rate=38400, require_rx=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_offline_timeout(config[CONF_OFFLINE_TIMEOUT]))
