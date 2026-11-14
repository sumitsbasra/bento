# Bento — To Do

## 🎨 GIF Animations (Assistant Screen)

Four looping GIFs are needed for the assistant screen states. Place them in the `bento/` sketch folder alongside `bento.ino`.

### GIFs needed

| File | State | Mood |
|---|---|---|
| `ambient.gif` | Idle | Slow, soft morphing gradient orb |
| `listening.gif` | Listening | Faster, brighter ripples / pulses |
| `thinking.gif` | Thinking | Rotating / swirling, slightly dimmer |
| `speaking.gif` | Speaking | Outward wave bursts |

**Specs:** 240×240px · infinite loop · ≤300KB each

### Convert GIFs → C arrays

Once you have the GIFs, run this from the `bento/` sketch folder:

```bash
for state in ambient listening thinking speaking; do
  xxd -i ${state}.gif \
    | sed "s/unsigned char ${state}\[\]/const uint8_t gif_${state}[] PROGMEM/; \
           s/unsigned int ${state}_len/const uint32_t gif_${state}_len/" \
    > gif_${state}.h
done
```

This overwrites the placeholder stubs in `gif_ambient.h`, `gif_listening.h`, `gif_thinking.h`, and `gif_speaking.h`. Recompile and upload.

### Install AnimatedGIF library

Arduino IDE → Library Manager → search **"AnimatedGIF"** by Larry Bank → Install

---

## 🔊 Audio (TODO from README)

- [ ] Implement I2S audio input (microphone)
- [ ] Implement I2S audio output (speaker playback)
- [ ] Add wake word detection
