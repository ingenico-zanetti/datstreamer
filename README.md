# datstreamer
Streams S16 stereo audio as a WAVE stream with configurable sample delay.
Input (typically from "arecord -f dat -t raw -D hw:CARD=H2n,DEV=0 - ") from stdin
Output to stdout or incoming TCP connections (configurable with a specific sample delay for each stream)
Hardcoded for 48kHz S16LE stereo stream
