#include "pcm_ring.h"
size_t PcmRingPack(const int16_t* ring, size_t capacity, uint32_t published,
                   uint32_t offset, uint32_t* stereo, size_t frames, unsigned volume) {
    if(!stereo)return 0;
    size_t available=published-offset;
    if(!ring || !capacity || available>capacity)available=0;
    size_t count=available<frames?available:frames;
    if(volume>100)volume=100;
    for(size_t i=0;i<frames;i++) {
        int32_t value=i<count?(int32_t)ring[(offset+i)%capacity]*(int32_t)volume/100:0;
        if(offset+i<80)value=value*(int32_t)(offset+i)/80;
        uint32_t bits=(uint16_t)(int16_t)value;
        stereo[i]=bits|(bits<<16);
    }
    return count;
}
