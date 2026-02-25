import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_SPEAKER,
    CONF_PORT,
    CONF_ICON,
    CONF_NAME,
    CONF_MODE,
    CONF_DISABLED_BY_DEFAULT,
)
from esphome.components import microphone, speaker, switch, text_sensor

CODEOWNERS = ["@n-IA-hane"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["switch", "number", "text_sensor"]

CONF_INTERCOM_API_ID = "intercom_api_id"
CONF_DC_OFFSET_REMOVAL = "dc_offset_removal"

CONF_AEC_ID = "aec_id"
CONF_RINGING_TIMEOUT = "ringing_timeout"
CONF_ON_RINGING = "on_ringing"
CONF_ON_STREAMING = "on_streaming"
CONF_ON_IDLE = "on_idle"
# FSM triggers
CONF_ON_OUTGOING_CALL = "on_outgoing_call"
CONF_ON_ANSWERED = "on_answered"
CONF_ON_HANGUP = "on_hangup"
CONF_ON_CALL_FAILED = "on_call_failed"

# Mode constants
MODE_SIMPLE = "simple"  # Simple: ring → HA notification → answer (browser ↔ ESP only)
MODE_FULL = "full"      # Full: contacts, destination, ESP↔ESP calls via HA bridge

intercom_api_ns = cg.esphome_ns.namespace("intercom_api")
IntercomApi = intercom_api_ns.class_("IntercomApi", cg.Component)

# === Action classes (for YAML: intercom_api.next_contact, etc.) ===
NextContactAction = intercom_api_ns.class_("NextContactAction", automation.Action)
PrevContactAction = intercom_api_ns.class_("PrevContactAction", automation.Action)
StartAction = intercom_api_ns.class_("StartAction", automation.Action)
StopAction = intercom_api_ns.class_("StopAction", automation.Action)
AnswerCallAction = intercom_api_ns.class_("AnswerCallAction", automation.Action)
DeclineCallAction = intercom_api_ns.class_("DeclineCallAction", automation.Action)
CallToggleAction = intercom_api_ns.class_("CallToggleAction", automation.Action)

# Parameterized actions
SetVolumeAction = intercom_api_ns.class_("SetVolumeAction", automation.Action)
SetMicGainDbAction = intercom_api_ns.class_("SetMicGainDbAction", automation.Action)
SetContactsAction = intercom_api_ns.class_("SetContactsAction", automation.Action)

# === Condition classes (for YAML: intercom_api.is_idle, etc.) ===
IntercomIsIdleCondition = intercom_api_ns.class_("IntercomIsIdleCondition", automation.Condition)
IntercomIsRingingCondition = intercom_api_ns.class_("IntercomIsRingingCondition", automation.Condition)
IntercomIsStreamingCondition = intercom_api_ns.class_("IntercomIsStreamingCondition", automation.Condition)
IntercomIsCallingCondition = intercom_api_ns.class_("IntercomIsCallingCondition", automation.Condition)
IntercomIsIncomingCondition = intercom_api_ns.class_("IntercomIsIncomingCondition", automation.Condition)
IntercomIsAnsweringCondition = intercom_api_ns.class_("IntercomIsAnsweringCondition", automation.Condition)
IntercomIsInCallCondition = intercom_api_ns.class_("IntercomIsInCallCondition", automation.Condition)

def _aec_schema(value):
    """Validate aec_id - import esp_aec only if used."""
    if value is None:
        return value
    # Import here to avoid circular dependency
    from esphome.components import esp_aec
    return cv.use_id(esp_aec.EspAec)(value)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(IntercomApi),
        # Mode: simple (browser↔ESP only) or full (multi-device with contacts, ESP↔ESP)
        cv.Optional(CONF_MODE, default=MODE_SIMPLE): cv.one_of(MODE_SIMPLE, MODE_FULL, lower=True),
        cv.Optional(CONF_MICROPHONE): microphone.microphone_source_schema(),
        cv.Optional(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        # DC offset removal for mics with significant DC bias (e.g., SPH0645)
        cv.Optional(CONF_DC_OFFSET_REMOVAL, default=False): cv.boolean,
        # Optional AEC (Acoustic Echo Cancellation) component
        cv.Optional(CONF_AEC_ID): _aec_schema,
        # Ringing timeout: auto-decline call if not answered within this time
        cv.Optional(CONF_RINGING_TIMEOUT): cv.positive_time_period_milliseconds,
        # Trigger when incoming call (auto_answer OFF)
        cv.Optional(CONF_ON_RINGING): automation.validate_automation(single=True),
        # Trigger when streaming starts
        cv.Optional(CONF_ON_STREAMING): automation.validate_automation(single=True),
        # Trigger when state returns to idle
        cv.Optional(CONF_ON_IDLE): automation.validate_automation(single=True),
        # FSM triggers
        cv.Optional(CONF_ON_OUTGOING_CALL): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_ANSWERED): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_HANGUP): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_CALL_FAILED): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mode = config[CONF_MODE]
    is_full = mode == MODE_FULL

    # Set mode on the C++ component
    cg.add(var.set_full_mode(is_full))

    if CONF_MICROPHONE in config:
        mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
        cg.add(var.set_microphone_source(mic_source))

    if CONF_SPEAKER in config:
        spk = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(spk))

    cg.add(var.set_dc_offset_removal(config[CONF_DC_OFFSET_REMOVAL]))

    # Set device name (for full mode: exclude self from contacts list)
    cg.add(var.set_device_name(cg.RawExpression('App.get_friendly_name()')))

    if CONF_AEC_ID in config and config[CONF_AEC_ID] is not None:
        aec = await cg.get_variable(config[CONF_AEC_ID])
        cg.add(var.set_aec(aec))
        cg.add_define("USE_ESP_AEC")

    # Ringing timeout (auto-decline if not answered)
    if CONF_RINGING_TIMEOUT in config:
        cg.add(var.set_ringing_timeout(config[CONF_RINGING_TIMEOUT]))

    # on_ringing automation
    if CONF_ON_RINGING in config:
        await automation.build_automation(
            var.get_ringing_trigger(), [], config[CONF_ON_RINGING]
        )

    # on_streaming automation
    if CONF_ON_STREAMING in config:
        await automation.build_automation(
            var.get_streaming_trigger(), [], config[CONF_ON_STREAMING]
        )

    # on_idle automation
    if CONF_ON_IDLE in config:
        await automation.build_automation(
            var.get_idle_trigger(), [], config[CONF_ON_IDLE]
        )

    # === FSM triggers ===
    if CONF_ON_OUTGOING_CALL in config:
        await automation.build_automation(
            var.get_outgoing_call_trigger(), [], config[CONF_ON_OUTGOING_CALL]
        )

    if CONF_ON_ANSWERED in config:
        await automation.build_automation(
            var.get_answered_trigger(), [], config[CONF_ON_ANSWERED]
        )

    # on_hangup with reason string argument
    if CONF_ON_HANGUP in config:
        await automation.build_automation(
            var.get_hangup_trigger(), [(cg.std_string, "reason")], config[CONF_ON_HANGUP]
        )

    # on_call_failed with reason string argument
    if CONF_ON_CALL_FAILED in config:
        await automation.build_automation(
            var.get_call_failed_trigger(), [(cg.std_string, "reason")], config[CONF_ON_CALL_FAILED]
        )

    # === Auto-create sensors ===

    # State sensor: always created (both simple and full modes need it)
    state_sensor_id = cv.declare_id(text_sensor.TextSensor)(f"{config[CONF_ID].id}_state")
    state_sensor = await text_sensor.new_text_sensor(
        {
            CONF_ID: state_sensor_id,
            CONF_NAME: "Intercom State",
            CONF_ICON: "mdi:phone-settings",
            CONF_DISABLED_BY_DEFAULT: False,
        }
    )
    cg.add(var.set_state_sensor(state_sensor))

    # Full mode sensors (contacts, destination, caller)
    if is_full:
        # Destination sensor (selected contact)
        dest_sensor_id = cv.declare_id(text_sensor.TextSensor)(f"{config[CONF_ID].id}_dest")
        dest_sensor = await text_sensor.new_text_sensor(
            {
                CONF_ID: dest_sensor_id,
                CONF_NAME: "Destination",
                CONF_ICON: "mdi:phone-forward",
                CONF_DISABLED_BY_DEFAULT: False,
            }
        )
        cg.add(var.set_destination_sensor(dest_sensor))

        # Caller name sensor (who is calling this device)
        caller_sensor_id = cv.declare_id(text_sensor.TextSensor)(f"{config[CONF_ID].id}_caller")
        caller_sensor = await text_sensor.new_text_sensor(
            {
                CONF_ID: caller_sensor_id,
                CONF_NAME: "Caller",
                CONF_ICON: "mdi:phone-incoming",
                CONF_DISABLED_BY_DEFAULT: False,
            }
        )
        cg.add(var.set_caller_sensor(caller_sensor))

        # Contacts list sensor (CSV of available contacts)
        contacts_sensor_id = cv.declare_id(text_sensor.TextSensor)(f"{config[CONF_ID].id}_contacts")
        contacts_sensor = await text_sensor.new_text_sensor(
            {
                CONF_ID: contacts_sensor_id,
                CONF_NAME: "Contacts",
                CONF_ICON: "mdi:account-group",
                CONF_DISABLED_BY_DEFAULT: False,
            }
        )
        cg.add(var.set_contacts_sensor(contacts_sensor))

    # NOTE: Auto-answer switch should be defined manually in YAML for proper restore behavior


# === Action registrations ===
# Simple action schema that just references the intercom_api component
INTERCOM_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(IntercomApi),
    }
)


@automation.register_action("intercom_api.next_contact", NextContactAction, INTERCOM_ACTION_SCHEMA)
async def next_contact_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_action("intercom_api.prev_contact", PrevContactAction, INTERCOM_ACTION_SCHEMA)
async def prev_contact_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_action("intercom_api.start", StartAction, INTERCOM_ACTION_SCHEMA)
async def start_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_action("intercom_api.stop", StopAction, INTERCOM_ACTION_SCHEMA)
async def stop_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_action("intercom_api.answer_call", AnswerCallAction, INTERCOM_ACTION_SCHEMA)
async def answer_call_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_action("intercom_api.decline_call", DeclineCallAction, INTERCOM_ACTION_SCHEMA)
async def decline_call_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_action("intercom_api.call_toggle", CallToggleAction, INTERCOM_ACTION_SCHEMA)
async def call_toggle_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


# === Parameterized actions ===

CONF_VOLUME = "volume"
CONF_GAIN_DB = "gain_db"
CONF_CONTACTS_CSV = "contacts_csv"


@automation.register_action(
    "intercom_api.set_volume",
    SetVolumeAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(IntercomApi),
            cv.Required(CONF_VOLUME): cv.templatable(cv.float_range(min=0.0, max=1.0)),
        }
    ),
)
async def set_volume_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    templ = await cg.templatable(config[CONF_VOLUME], args, float)
    cg.add(var.set_volume(templ))
    return var


@automation.register_action(
    "intercom_api.set_mic_gain_db",
    SetMicGainDbAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(IntercomApi),
            cv.Required(CONF_GAIN_DB): cv.templatable(cv.float_range(min=-20.0, max=20.0)),
        }
    ),
)
async def set_mic_gain_db_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    templ = await cg.templatable(config[CONF_GAIN_DB], args, float)
    cg.add(var.set_gain_db(templ))
    return var


@automation.register_action(
    "intercom_api.set_contacts",
    SetContactsAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(IntercomApi),
            cv.Required(CONF_CONTACTS_CSV): cv.templatable(cv.string),
        }
    ),
)
async def set_contacts_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    templ = await cg.templatable(config[CONF_CONTACTS_CSV], args, cg.std_string)
    cg.add(var.set_contacts_csv(templ))
    return var


# === Condition registrations ===
# Simple condition schema that just references the intercom_api component
INTERCOM_CONDITION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(IntercomApi),
    }
)


@automation.register_condition(
    "intercom_api.is_idle", IntercomIsIdleCondition, INTERCOM_CONDITION_SCHEMA
)
async def intercom_is_idle_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_condition(
    "intercom_api.is_ringing", IntercomIsRingingCondition, INTERCOM_CONDITION_SCHEMA
)
async def intercom_is_ringing_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_condition(
    "intercom_api.is_streaming", IntercomIsStreamingCondition, INTERCOM_CONDITION_SCHEMA
)
async def intercom_is_streaming_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_condition(
    "intercom_api.is_calling", IntercomIsCallingCondition, INTERCOM_CONDITION_SCHEMA
)
async def intercom_is_calling_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_condition(
    "intercom_api.is_incoming", IntercomIsIncomingCondition, INTERCOM_CONDITION_SCHEMA
)
async def intercom_is_incoming_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_condition(
    "intercom_api.is_answering", IntercomIsAnsweringCondition, INTERCOM_CONDITION_SCHEMA
)
async def intercom_is_answering_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var


@automation.register_condition(
    "intercom_api.is_in_call", IntercomIsInCallCondition, INTERCOM_CONDITION_SCHEMA
)
async def intercom_is_in_call_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    return var
