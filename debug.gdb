target remote :3333
set remotetimeout 10
symbol-file /home/daniele/cc/intercom-api/.esphome/build/esphome-web-0bac48/.pioenvs/esphome-web-0bac48/firmware.elf

# Get addresses for direct memory reads later
print &i2s_duplex->last_speaker_audio_ms_
print &i2s_duplex->aec_frame_count_
print &i2s_duplex->speaker_running_
print &i2s_duplex->speaker_buffer_

# Also read current values
print i2s_duplex->speaker_buffer_
print i2s_duplex->last_speaker_audio_ms_
print i2s_duplex->aec_frame_count_
print i2s_duplex->speaker_running_

# Resume immediately
monitor resume
