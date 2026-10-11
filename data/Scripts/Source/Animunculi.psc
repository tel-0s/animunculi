Scriptname Animunculi Hidden

; Animunculi: embodied AI agents. Native functions provided by Animunculi.dll (SKSE).
;
; A follower mod typically:
;   1. Binds its follower on init and after load:  Animunculi.Bind(FollowerRef, "default")
;   2. Forwards player input to the agent:         Animunculi.PlayerSay("Where to next?")
;   3. Offers extra verbs to the agent:
;        Animunculi.RegisterAction("dance", "Dance a short jig", "{}")
;        RegisterForModEvent("Animunculi_Action_dance", "OnDance")
;        Event OnDance(string eventName, string argsJson, float actionId, Form agentActor)
;            ; ... do the thing ...
;            Animunculi.CompleteAction(actionId as int, true, "{\"danced\":true}")
;        EndEvent
;
; Mod events sent by the plugin (RegisterForModEvent):
;   Animunculi_SpeechStarted  (strArg = line, numArg = duration in seconds, sender = speaking actor)
;   Animunculi_SpeechFinished (strArg = line, numArg = 1 if interrupted, sender = speaking actor)
;   Animunculi_Action_<name>  (strArg = JSON arguments, numArg = action id, sender = agent actor)
;   Animunculi_Thinking       (strArg = agent id, numArg = 1 while the agent's model is thinking, 0 after)

; ---- embodiment -------------------------------------------------------------------------------
; Makes akActor the body of the agent with id asAgent (one body per agent, one agent per body).
; The binding is saved in the SKSE co-save.
bool Function Bind(Actor akActor, string asAgent = "default") global native
Function Unbind(string asAgent = "default") global native
Actor Function GetBoundActor(string asAgent = "default") global native
; The agent id inhabiting akActor, or "" if none.
string Function GetAgentId(Actor akActor) global native

; True while at least one agent client (e.g. the MCP server) is connected.
bool Function IsConnected() global native
bool Function IsSpeaking(string asAgent = "default") global native

; ---- player -> agent ---------------------------------------------------------------------------
; Delivers text to agents as a player_chat event (asAgent = "" addresses everyone).
Function PlayerSay(string asText, string asAgent = "") global native
; Emits a custom event (asData may be JSON) that agents receive as {type: "custom", kind, data}.
Function EmitEvent(string asKind, string asData = "") global native

; ---- mod-provided actions ----------------------------------------------------------------------
Function RegisterAction(string asName, string asDescription, string asParamsHint = "") global native
Function UnregisterAction(string asName) global native
Function CompleteAction(int aiActionId, bool abSuccess, string asResult = "") global native

; ---- misc ---------------------------------------------------------------------------------------
string Function GetVersion() global native
; Opens the chat window (requires SKSE Menu Framework).
Function OpenChat() global native
