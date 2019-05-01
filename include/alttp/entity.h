#ifndef ALTTP_ENTITY_H
#define ALTTP_ENTITY_H

enum EntityID {
    BigPurpleGuard = 0x41,
    BigGreenGuard = 0x42,
    HomingGuard = 0x44,
    SmallGreenGuard = 0x4b,
    SpikeBallGuard = 0x6a,
    Aghanim = 0x7a,
    Heart_d8 = 0xd8,
    GreenRupee = 0xd9,
    BlueRupee = 0xda,
    RedRupee = 0xdb,
    OneBombDrop = 0xdc,
    Heart_dd = 0xdd,
    EightBombsDrop = 0xde,
    Heart_df = 0xdf,
    SmallMagicPotion = 0xe0,
    FiveArrowsDrop = 0xe1,
    TenArrowsDrop = 0xe2,
    Fairy = 0xe3,
    SmallKey = 0xe4,
    BossKey = 0xe5
};

extern const char* EntityNames[256];

#endif
