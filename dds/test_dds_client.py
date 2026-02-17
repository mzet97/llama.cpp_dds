#!/usr/bin/env python3
"""
DDS Client Python - Test with Phi-4-mini model via CycloneDDS
Sends requests via DDS and receives responses via DDS
"""

import time
import uuid
import sys
import os
import ctypes
from ctypes import *
import threading

# Load CycloneDDS library
libdds = CDLL("libddsc.so.0")

# DDS types matching the IDL structure
class ChatMessage(Structure):
    _fields_ = [("role", c_char_p),
                ("content", c_char_p)]

class dds_sequence_llama_ChatMessage(Structure):
    _fields_ = [("_maximum", c_uint32),
                ("_length", c_uint32),
                ("_buffer", POINTER(ChatMessage)),
                ("_release", c_bool)]

class dds_sequence_float(Structure):
    _fields_ = [("_maximum", c_uint32),
                ("_length", c_uint32),
                ("_buffer", POINTER(c_float)),
                ("_release", c_bool)]

class dds_sequence_long(Structure):
    _fields_ = [("_maximum", c_uint32),
                ("_length", c_uint32),
                ("_buffer", POINTER(c_int32)),
                ("_release", c_bool)]

class dds_sequence_string(Structure):
    _fields_ = [("_maximum", c_uint32),
                ("_length", c_uint32),
                ("_buffer", POINTER(c_char_p)),
                ("_release", c_bool)]

class ChatCompletionRequest(Structure):
    _fields_ = [("request_id", c_char_p),
                ("model", c_char_p),
                ("messages", dds_sequence_llama_ChatMessage),
                ("temperature", c_float),
                ("max_tokens", c_int32),
                ("stream", c_bool),
                ("top_p", dds_sequence_float),
                ("n", dds_sequence_long),
                ("stop", dds_sequence_string)]

class ChatCompletionResponse(Structure):
    _fields_ = [("request_id", c_char_p),
                ("model", c_char_p),
                ("content", c_char_p),
                ("finish_reason", c_char_p),
                ("is_final", c_bool),
                ("prompt_tokens", c_int32),
                ("completion_tokens", c_int32)]

# DDS functions
dds_create_participant = libdds.dds_create_participant
dds_create_participant.argtypes = [c_int, POINTER(None), POINTER(None)]
dds_create_participant.restype = c_int

dds_create_topic = libdds.dds_create_topic
dds_create_topic.argtypes = [c_int, POINTER(None), c_char_p, POINTER(None), POINTER(None)]
dds_create_topic.restype = c_int

dds_create_writer = libdds.dds_create_writer
dds_create_writer.argtypes = [c_int, c_int, POINTER(None), POINTER(None)]
dds_create_writer.restype = c_int

dds_create_reader = libdds.dds_create_reader
dds_create_reader.argtypes = [c_int, c_int, POINTER(None), POINTER(None), POINTER(None)]
dds_create_reader.restype = c_int

dds_write = libdds.dds_write
dds_write.argtypes = [c_int, POINTER(None)]
dds_write.restype = c_int

dds_take = libdds.dds_take
dds_take.argtypes = [c_int, POINTER(c_void_p), POINTER(None), c_size_t, c_size_t]
dds_take.restype = c_int

dds_read = libdds.dds_read
dds_read.argtypes = [c_int, POINTER(c_void_p), POINTER(None), c_size_t, c_size_t]
dds_read.restype = c_int

dds_strretcode = libdds.dds_strretcode
dds_strretcode.argtypes = [c_int]
dds_strretcode.restype = c_char_p

dds_delete = libdds.dds_delete
dds_delete.argtypes = [c_int]
dds_delete.restype = c_int

dds_alloc = libdds.dds_alloc
dds_alloc.argtypes = [c_size_t]
dds_alloc.restype = c_void_p

dds_free = libdds.dds_free
dds_free.argtypes = [c_void_p]
dds_free.restype = None

# Topic names
TOPIC_REQUEST = b"llama_chat_completion_request"
TOPIC_RESPONSE = b"llama_chat_completion_response"

# Topic descriptors (from generated code - we'll use strings)
# For simplicity, we'll try to get them dynamically

def main():
    domain_id = 0 if len(sys.argv) < 2 else int(sys.argv[1])

    print("=" * 60)
    print("DDS Client Python - Phi-4-mini Test")
    print("=" * 60)
    print(f"Domain: {domain_id}")
    print()

    # Create participant
    participant = dds_create_participant(domain_id, None, None)
    if participant < 0:
        print(f"Error creating participant: {dds_strretcode(participant).decode()}")
        return 1

    print("Participant created")

    # Get topic descriptors - we need to link with the generated code
    # For now, let's try a simpler approach using the topic names
    # The descriptors are defined in the generated C code

    # Try to load the descriptor from the shared library
    try:
        # These are exported by the IDL-generated code
        request_desc = c_void_p.in_dll(libdds, "llama_ChatCompletionRequest_desc")
        response_desc = c_void_p.in_dll(libdds, "llama_ChatCompletionResponse_desc")
        print("Found topic descriptors in library")
    except:
        print("ERROR: Could not find topic descriptors in library")
        print("The IDL-generated code must be linked")
        dds_delete(participant)
        return 1

    # Create topics
    request_topic = dds_create_topic(participant, request_desc, TOPIC_REQUEST, None, None)
    if request_topic < 0:
        print(f"Error creating request topic: {dds_strretcode(request_topic).decode()}")
        dds_delete(participant)
        return 1

    response_topic = dds_create_topic(participant, response_desc, TOPIC_RESPONSE, None, None)
    if response_topic < 0:
        print(f"Error creating response topic: {dds_strretcode(response_topic).decode()}")
        dds_delete(participant)
        return 1

    print(f"Topics created: request={request_topic}, response={response_topic}")

    # Create writer for requests
    request_writer = dds_create_writer(participant, request_topic, None, None)
    if request_writer < 0:
        print(f"Error creating request writer: {dds_strretcode(request_writer).decode()}")
        dds_delete(participant)
        return 1

    # Create reader for responses
    response_reader = dds_create_reader(participant, response_topic, None, None, None)
    if response_reader < 0:
        print(f"Error creating response reader: {dds_strretcode(response_reader).decode()}")
        dds_delete(participant)
        return 1

    print(f"Writer={request_writer}, Reader={response_reader}")
    print()

    # Generate a unique request ID
    request_id = str(uuid.uuid4()).encode()

    # Prepare the request
    prompt = "Some os 4 primeiros números deste ano (2026)."
    print(f"Prompt: {prompt}")
    print(f"Request ID: {request_id.decode()}")
    print()

    # Allocate request structure
    req = ChatCompletionRequest()
    req.request_id = request_id
    req.model = b"phi4-mini"
    req.temperature = 0.3
    req.max_tokens = 50
    req.stream = False

    # Set up messages - one message
    msg = ChatMessage()
    msg.role = b"user"
    msg.content = prompt.encode()

    # Allocate sequence for messages
    msg_seq = dds_sequence_llama_ChatMessage()
    msg_seq._maximum = 1
    msg_seq._length = 1
    msg_seq._buffer = (ChatMessage * 1)(msg)
    msg_seq._release = False
    req.messages = msg_seq

    # Empty optional sequences
    req.top_p._maximum = 0
    req.top_p._length = 0
    req.top_p._buffer = None
    req.top_p._release = False

    req.n._maximum = 0
    req.n._length = 0
    req.n._buffer = None
    req.n._release = False

    req.stop._maximum = 0
    req.stop._length = 0
    req.stop._buffer = None
    req.stop._release = False

    # Send request via DDS
    print("Sending request via DDS...")
    start_time = time.time()

    ret = dds_write(request_writer, byref(req))
    if ret < 0:
        print(f"Error writing request: {dds_strretcode(ret).decode()}")
        dds_delete(participant)
        return 1

    print("Request sent, waiting for response...")

    # Wait for response
    response_received = False
    timeout = 60  # seconds
    elapsed = 0

    while elapsed < timeout and not response_received:
        samples = (c_void_p * 1)()
        infos = (c_void_p * 1)()

        n = dds_read(response_reader, samples, infos, 1, 1)

        if n > 0:
            # Check if this is our response
            resp = cast(samples[0], POINTER(ChatCompletionResponse)).contents
            if resp.request_id == request_id:
                end_time = time.time()
                elapsed = end_time - start_time

                print()
                print("=" * 60)
                print("Response received!")
                print(f"Time elapsed: {elapsed:.2f} seconds")
                print("=" * 60)
                print(f"Content: {resp.content.decode() if resp.content else ''}")
                print(f"Finish reason: {resp.finish_reason.decode() if resp.finish_reason else 'N/A'}")
                print(f"Prompt tokens: {resp.prompt_tokens}")
                print(f"Completion tokens: {resp.completion_tokens}")
                print(f"is_final: {resp.is_final}")
                print("=" * 60)

                response_received = True
        else:
            time.sleep(0.1)
            elapsed = time.time() - start_time

    if not response_received:
        print(f"Timeout waiting for response after {timeout} seconds")

    # Cleanup
    dds_delete(participant)

    print("\nDone!")
    return 0

if __name__ == "__main__":
    sys.exit(main())
