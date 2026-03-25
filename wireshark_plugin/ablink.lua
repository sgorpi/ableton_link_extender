-- Wireshark Dissector for the Ableton Link protocol
--
--  Copyright (C) 2026  Hedde Bosman <sgorpi at gmail dot com>
--
--  This program is free software: you can redistribute it and/or modify
--  it under the terms of the GNU General Public License as published by
--  the Free Software Foundation, either version 3 of the License, or
--  (at your option) any later version.
--
--  This program is distributed in the hope that it will be useful,
--  but WITHOUT ANY WARRANTY; without even the implied warranty of
--  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
--  GNU General Public License for more details.
--
--  You should have received a copy of the GNU General Public License
--  along with this program.  If not, see <https://www.gnu.org/licenses/>.
--
-- 
-- This dissector is designed to dissect Ableton Link packets, which are used for synchronization between
-- several Digital Audio Workstations (DAWs) like Ableton Live, Logic or Bitwig and other music software.


local dprint = function(...)
    print(table.concat({"Lua:", ...}," "))
end

-- create the "protocol" first
local link_protocol = Proto("ablink", "Ableton Link")

local protocol_fields = {
    -- header
    protocol_version = ProtoField.uint8("ablink.version", "Protocol Version"),
    message_type = ProtoField.uint8("ablink.message_type", "Message Type"),
    ttl = ProtoField.uint8("ablink.ttl", "TTL"),
    group_id = ProtoField.uint16("ablink.group_id", "Group ID"),
    from_node_id = ProtoField.bytes("ablink.from_node_id", "From Node ID"),
    -- shared between state/measurement
    session_id = ProtoField.bytes("ablink.session_id", "Session ID"),
    -- state
    microseconds_per_beat = ProtoField.int64("ablink.tmln.microseconds_per_beat", "Microseconds per beat"),
    beat_origin = ProtoField.int64("ablink.tmln.beat", "Beat Origin"),
    time_origin = ProtoField.int64("ablink.tmln.time", "Time Origin"),
    state_playing = ProtoField.bool("ablink.state.playing", "Playing"),
    state_beat = ProtoField.int64("ablink.state.beat", "Beat"),
    state_time = ProtoField.int64("ablink.state.time", "Time"),
    measurement_endpoint_ipv4 = ProtoField.ipv4("ablink.meas.v4", "Measurement Endpoint IPv4"),
    measurement_endpoint_ipv6 = ProtoField.ipv6("ablink.meas.v6", "Measurement Endpoint IPv6"),
    measurement_port = ProtoField.uint16("ablink.meas.port", "Measurement Port"),
    -- measurement
    hosttime = ProtoField.int64("ablink.meas.hosttime", "Host Time"),
    prevglobaltime = ProtoField.int64("ablink.meas.prevglobaltime", "Previous Global Time"),
    globaltime = ProtoField.int64("ablink.meas.globaltime", "Global Time"),
}

link_protocol.fields = protocol_fields

local PROTOCOL_NAME_SHORT="AbletonLink"
local PROTOCOL_NAME_LONG = "Ableton Link v"

local message_type_to_string = {
    [0x01] = "Keep Alive",
    [0x02] = "Response  ",
    [0x03] = "Bye Bye",
}
local play_state_to_string = {
    [0] = "Stopped",
    [1] = "Playing",
}


function node_id_to_string(node_id)
    return string.format("%04x%04x%04x%04x", 
        node_id:range(0,2):uint(),
        node_id:range(2,2):uint(),
        node_id:range(4,2):uint(),
        node_id:range(6,2):uint()
    )
end

function link_protocol.dissector(tvb, pinfo, tree)
    local pos = 0
    local pktlen = tvb:reported_length_remaining()
    -- protocol = UDP (3) and destination port = 20808
    if tvb:range(0,7):raw() == "_asdp_v" and pktlen > 19 then
        pos = pos+7
        -- packet should start with "_asdp_v."
        local protocol_version = tvb:range(pos, 1)
        pos = pos + 1
        
        
        local message_type = tvb:range(pos, 1)
        pos = pos + 1
        
        local ttl = tvb:range(pos, 1)
        pos = pos + 1
        
        local group_id = tvb:range(pos, 2)
        pos = pos + 2
        
        local from_node_id = tvb:range(pos, 8)
        pos = pos + 8

        local subtree = tree:add(link_protocol, string.format("%s%d, from %s",
            PROTOCOL_NAME_LONG,
            protocol_version:int(), 
            node_id_to_string(from_node_id)))
        subtree:add(protocol_fields.protocol_version, protocol_version)
        subtree:add(protocol_fields.message_type, message_type, message_type:uint(), nil, string.format("(%s)", message_type_to_string[message_type:uint()]))
        subtree:add(protocol_fields.ttl, ttl)
        subtree:add(protocol_fields.group_id, group_id)
        subtree:add(protocol_fields.from_node_id, from_node_id)

        --dprint(string.format("Header until %d", pos)) -- 20

        if message_type:uint() ~= 0x03 then
            -- timeline starts with "tmln", then microseconds_per_beat (int64), beatOrigin(int64), timeOrigin(int64) in microseconds.
            if tvb:range(pos, 4):raw() ~= "tmln" then
                dprint(string.format("expected 'tmln' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
            end
            pos = pos + 4
            pos = pos + 4 -- four bytes extra that indicate the size of the timeline

            local microseconds_per_beat = tvb:range(pos, 8)
            local bpm = ((60*1000000) / microseconds_per_beat:int64():tonumber())
            --dprint(string.format("BPM: %f", bpm))

            pos = pos + 8
            local beat_origin = tvb:range(pos, 8)
            pos = pos + 8
            local time_origin = tvb:range(pos, 8)
            pos = pos + 8
            local origin = string.format("beat=%s time=%s", 
                beat_origin:int64(), 
                time_origin:int64())
            --dprint(string.format("Timeline until %d", pos)) -- 44
                
            -- session id starts with "sess", then a node_id type
            if tvb:range(pos, 4):raw() ~= "sess" then
                dprint(string.format("expected 'sess' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
            end
            pos = pos + 4 
            pos = pos + 4 -- four bytes extra that indicate the size of the session_id (8)
            local session_id = tvb:range(pos, 8)
            pos = pos + 8

            --dprint(string.format("Session until %d", pos)) -- 52
            
            -- start/stop state starts with "stst", then a bool for isPlaying, Beats(int64) and Timestamp (int64),
            if tvb:range(pos, 4):raw() ~= "stst" then
                dprint(string.format("expected 'stst' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
            end
            pos = pos + 4
            pos = pos + 4 -- four bytes extra that indicate the size of the start/stop state
            local is_playing = tvb:range(pos, 1)
            pos = pos + 1
            local beats = tvb:range(pos, 8)
            pos = pos + 8
            local timestamp = tvb:range(pos, 8)
            pos = pos + 8

            --dprint(string.format("Start/Stop/State until %d", pos)) -- 69

            subtree:add(protocol_fields.microseconds_per_beat, microseconds_per_beat, microseconds_per_beat:int64(), nil, string.format("(%.3f BPM)", bpm))
            subtree:add(protocol_fields.beat_origin, beat_origin)
            subtree:add(protocol_fields.time_origin, time_origin)

            subtree:add(protocol_fields.session_id, session_id)

            subtree:add(protocol_fields.state_playing, is_playing)
            subtree:add(protocol_fields.state_beat, beats)
            subtree:add(protocol_fields.state_time, timestamp)

            -- measurement endpoint, starts with 'mep4' or 'mep6' depending on IPv4/IPv6
            local measurement_endpoint_type = tvb:range(pos, 4):raw()
            pos = pos + 4
            pos = pos + 4 -- four bytes extra that indicate the size of the measurement endpoint
            local measurement_endpoint = string.format("unknown at %d: '%s'", pos, measurement_endpoint_type)
            if measurement_endpoint_type == "mep4" then
                local measurement_endpoint_ipv4 = tvb:range(pos, 4)
                pos = pos + 4
                subtree:add(protocol_fields.measurement_endpoint_ipv4, measurement_endpoint_ipv4)
            elseif measurement_endpoint_type == "mep6" then
                measurement_endpoint_ipv6 = tvb:range(pos, 16)
                pos = pos + 16
                subtree:add(protocol_fields.measurement_endpoint_ipv6, measurement_endpoint_ipv6)
            end
            local measurement_port = tvb:range(pos, 2)
            pos = pos + 2

            --dprint(string.format("Measurement endpoint until %d", pos)) -- 

            subtree:add(protocol_fields.measurement_port, measurement_port)
            pinfo.cols.info = string.format("Node %s  %s  %s, %.3f BPM, Beat=%s, Time=%s", 
                node_id_to_string(from_node_id),
                message_type_to_string[message_type:uint()],
                play_state_to_string[is_playing:uint()],
                bpm,
                beats:int64(),
                timestamp:int64()
            )
        else
            pinfo.cols.info = string.format("node %s  %s", 
                node_id_to_string(from_node_id),
                message_type_to_string[message_type:uint()])
        end
        pinfo.cols.protocol = PROTOCOL_NAME_SHORT

    elseif tvb:range(0,7):raw() == "_link_v" and pktlen > 24 then
        pos = pos + 7
        -- packet should start with "_link_v."
        local protocol_version = tvb:range(pos, 1)
        pos = pos + 1
        local message_type = tvb:range(pos, 1)
        pos = pos + 1

        local subtree = tree:add(link_protocol, string.format("%s%d", PROTOCOL_NAME_LONG, protocol_version:int()))
        if message_type:uint() == 0x01 then -- ping
            -- host time starts with "__ht", then hosttime(int64) in microseconds.
            if tvb:range(pos, 4):raw() ~= "__ht" then
                dprint(string.format("expected '__ht' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
            end
            pos = pos + 4
            pos = pos + 4 -- four bytes extra that indicate the size of the host time

            local hosttime = tvb:range(pos, 8)
            pos = pos + 8

            subtree:add(protocol_fields.protocol_version, protocol_version)
            subtree:add(protocol_fields.message_type, message_type, message_type:uint(), nil, "(Ping)")
            subtree:add(protocol_fields.hosttime, hosttime)

            if pktlen > pos+7 then
                -- previous global host time starts with "_pgt", then globaltime(int64) in microseconds.
                if tvb:range(pos, 4):raw() ~= "_pgt" then
                    dprint(string.format("expected '_pgt' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
                end
                pos = pos + 4
                pos = pos + 4 -- four bytes extra that indicate the size of the host time

                local prevglobaltime = tvb:range(pos, 8)
                pos = pos + 8

                subtree:add(protocol_fields.prevglobaltime, prevglobaltime)
                pinfo.cols.info = string.format("Ping, Host-time=%s, Previous-time=%s",
                    hosttime:int64(),
                    prevglobaltime:int64())
            else
                pinfo.cols.info = string.format("Ping, Host-time=%s",
                    hosttime:int64())
            end


        elseif message_type:uint() == 0x02 then -- pong
            -- session id starts with "sess", then a node_id type
            if tvb:range(pos, 4):raw() ~= "sess" then
                dprint(string.format("expected 'sess' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
            end
            pos = pos + 4 
            pos = pos + 4 -- four bytes extra that indicate the size of the session_id (8)
            local session_id = tvb:range(pos, 8)
            pos = pos + 8

            -- global time starts with "__gt", then globaltime(int64) in microseconds.
            if tvb:range(pos, 4):raw() ~= "__gt" then
                dprint(string.format("expected '__gt' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
            end
            pos = pos + 4
            pos = pos + 4 -- four bytes extra that indicate the size of the global time
            local globaltime = tvb:range(pos, 8)
            pos = pos + 8

            -- host time starts with "__ht", then hosttime(int64) in microseconds.
            if tvb:range(pos, 4):raw() ~= "__ht" then
                dprint(string.format("expected '__ht' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
            end
            pos = pos + 4
            pos = pos + 4 -- four bytes extra that indicate the size of the host time
            local hosttime = tvb:range(pos, 8)
            pos = pos + 8

            subtree:add(protocol_fields.protocol_version, protocol_version)
            subtree:add(protocol_fields.message_type, message_type, message_type:uint(), nil, "(Pong)")
            subtree:add(protocol_fields.hosttime, hosttime)
            subtree:add(protocol_fields.session_id, session_id)
            subtree:add(protocol_fields.globaltime, globaltime)
            if pktlen > pos+7 then
                -- previous global host time starts with "_pgt", then globaltime(int64) in microseconds.
                if tvb:range(pos, 4):raw() ~= "_pgt" then
                    dprint(string.format("expected '_pgt' at %d, got '%s'", pos, tvb:range(pos, 4):raw()))
                end
                pos = pos + 4
                pos = pos + 4 -- four bytes extra that indicate the size of the host time

                local prevglobaltime = tvb:range(pos, 8)
                pos = pos + 8

                subtree:add(protocol_fields.prevglobaltime, prevglobaltime)

                pinfo.cols.info = string.format("Pong, Host-time=%s, Previous-time=%s, Global-time=%s, Session=%s ",
                    hosttime:int64(), 
                    prevglobaltime:int64(),
                    globaltime:int64(),
                    node_id_to_string(session_id))
            else
                pinfo.cols.info = string.format("Pong, Host-time=%s, Global-time=%s, Session=%s ",
                    hosttime:int64(), 
                    globaltime:int64(),
                    node_id_to_string(session_id))
            end

        end
        pinfo.cols.protocol = PROTOCOL_NAME_SHORT
    else
        dprint(string.format("unknown protocol %s", tvb:range(0,7):raw()))
    end
    return pos
end

local function heur_dissect_ableton_link(tvbuf,pktinfo,root)
    if tvbuf:len() < 8 then
        return false
    end
    if tvbuf:range(0,7):raw() ~= "_asdp_v" and tvbuf:range(0,7):raw() ~= "_link_v" then
        return false
    end
    link_protocol.dissector(tvbuf,pktinfo,root)
    pktinfo.conversation = link_protocol
    return true
end
-- register the protocol
-- register_postdissector(link_protocol)
DissectorTable.get("udp.port"):add(20808, link_protocol)
link_protocol:register_heuristic("udp", heur_dissect_ableton_link)