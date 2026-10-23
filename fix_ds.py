import re

with open(r'C:\XboxXBERecomp\runtime\xdk\dsound.cpp', 'r') as f:
    content = f.read()

old = """uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;"""

new = """uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x100);  // larger allocation for more fields
    std::memset(HostPtr(obj), 0, 0x100);
    // use the game's actual IDirectSoundBuffer vtable
    constexpr uint32_t kGameVoiceVtable = 0x3C06D4;  // SYM_IDirectSoundBuffer_Release - 8
    W32(obj, kGameVoiceVtable);
    // initialize common IDirectSoundBuffer fields in guest memory
    W32(obj + 4, 1);        // refcount = 1
    W32(obj + 8, g_ds);     // parent IDirectSound pointer
    W32(obj + 0xC, 0);      // format pointer (null)
    W32(obj + 0x10, 0);     // buffer pointer
    W32(obj + 0x14, 0);     // buffer size
    W32(obj + 0x18, 0);     // play region start
    W32(obj + 0x1C, 0);     // play region length
    W32(obj + 0x20, 0);     // loop region start
    W32(obj + 0x1C, 0);     // loop region length
    W32(obj + 0x24, 0);     // current play position
    W32(obj + 0x28, 0);     // write position
    W32(obj + 0x2C, 0);     // notify positions
    W32(obj + 0x30, 0);     // notify count
    W32(obj + 0x34, 0);     // 3D algorithm
    W32(obj + 0x38, 0);     // 3D position
    W32(obj + 0x3C, 0);     // 3D velocity
    W32(obj + 0x40, 0);     // 3D cone
    W32(obj + 0x44, 0);     // 3D min/max distance
    W32(obj + 0x48, 0);     // 3D mode
    W32(obj + 0x4C, 0);     // 3D rolloff factor
    W32(obj + 0x50, 0);     // 3D max distance
    W32(obj + 0x54, 0);     // 3D min distance
    W32(obj + 0x58, 0);     // 3D cone orientation
    W32(obj + 0x5C, 0);     // 3D cone angle
    W32(obj + 0x60, 0);     // 3D cone outside volume
    W32(obj + 0x64, 0);     // mix bins
    W32(obj + 0x68, 0);     // mix bin volumes
    W32(obj + 0x6C, 0);     // effects
    W32(obj + 0x70, 0);     // effect description
    W32(obj + 0x74, 0);     // effect image
    W32(obj + 0x78, 0);     // effect param
    W32(obj + 0x7C, 0);     // effect next
    W32(obj + 0x80, 0);     // 3D algorithm
    W32(obj + 0x84, 0);     // 3D params
    W32(obj + 0x88, 0);     // 3D position
    W32(obj + 0x8C, 0);     // 3D velocity
    W32(obj + 0x90, 0);     // 3D cone
    W32(obj + 0x94, 0);     // 3D min/max distance
    W32(obj + 0x98, 0);     // 3D mode
    W32(obj + 0x9C, 0);     // 3D rolloff factor
    W32(obj + 0xA0, 0);     // 3D max distance
    W32(obj + 0xA4, 0);     // 3D min distance
    W32(obj + 0xA8, 0);     // 3D cone orientation
    W32(obj + 0xAC, 0);     // 3D cone angle
    W32(obj + 0xB0, 0);     // 3D cone outside volume
    W32(obj + 0xB4, 0);     // mix bins
    W32(obj + 0xB8, 0);     // mix bin volumes
    W32(obj + 0xBC, 0);     // effects
    W32(obj + 0xC0, 0);     // effect description
    W32(obj + 0xC4, 0);     // effect image
    W32(obj + 0xC8, 0);     // effect param
    W32(obj + 0xCC, 0);     // effect next
    W32(obj + 0xD0, 0);     // 3D algorithm
    W32(obj + 0xD4, 0);     // 3D params
    W32(obj + 0xD8, 0);     // 3D position
    W32(obj + 0xDC, 0);     // 3D velocity
    W32(obj + 0xE0, 0);     // 3D cone
    W32(obj + 0xE4, 0);     // 3D min/max distance
    W32(obj + 0xE8, 0);     // 3D mode
    W32(obj + 0xEC, 0);     // 3D rolloff factor
    W32(obj + 0xF0, 0);     // 3D max distance
    W32(obj + 0xF4, 0);     // 3D min distance
    W32(obj + 0xF8, 0);     // 3D cone orientation
    W32(obj + 0xFC, 0);     // 3D cone angle
    Voice& v = g_voices[obj];
    v.vtable = 0x3C06D4;  // SYM_IDirectSoundBuffer_Release - 8
    v.obj = obj;"""

content = content.replace(
    "uint32_t NewBuffer(uint32_t desc, uint32_t out) {\n    std::lock_guard<std::recursive_mutex> lock(g_lock);\n    Start();\n    uint32_t obj = mem::AllocSystem(0x40);\n    std::memset(HostPtr(obj), 0, 0x40);\n    Voice& v = g_voices[obj];\n    v.obj = obj;",
    new
)

with open(r'C:\XboxXBERecomp\runtime\xdk\dsound.cpp', 'w') as f:
    f.write(content)

print('Done')