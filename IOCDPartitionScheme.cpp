/*
 * Copyright (c) 1998-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <IOKit/assert.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOCDPartitionScheme.h>

#define super IOPartitionScheme
OSDefineMetaClassAndStructors(IOCDPartitionScheme, IOPartitionScheme);

#define kIOCDPartitionSchemeContentTable "Content Table"

IOCDMedia * IOCDPartitionScheme::getProvider() const
{
    //
    // Obtain this object's provider.  We override the superclass's method
    // to return a more specific subclass of OSObject -- IOCDMedia.   This
    // method serves simply as a convenience to subclass developers.
    //

    return (IOCDMedia *) IOService::getProvider();
}

bool IOCDPartitionScheme::init(OSDictionary * properties)
{
    //
    // Initialize this object's minimal state.
    //

    // State our assumptions.

    assert(sizeof(CDTOC)           ==  4);          // (compiler/platform check)
    assert(sizeof(CDTOCDescriptor) == 11);          // (compiler/platform check)

    // Ask our superclass' opinion.

    if (super::init(properties) == false)  return false;

    // Initialize our state.

    _partitions = 0;

    return true;
}

void IOCDPartitionScheme::free()
{
    //
    // Free all of this object's outstanding resources.
    //

    if ( _partitions )  _partitions->release();

    super::free();
}

IOService * IOCDPartitionScheme::probe(IOService * provider, SInt32 * score)
{
    //
    // Determine whether the provider media contains CD partitions.
    //

    // State our assumptions.

    assert(OSDynamicCast(IOCDMedia, provider));

    // Ask superclass' opinion.

    if (super::probe(provider, score) == 0)  return 0;

    // Scan the provider media for CD partitions.

    _partitions = scan(score);

    return ( _partitions ) ? this : 0;
}

bool IOCDPartitionScheme::start(IOService * provider)
{
    //
    // Publish the new media objects which represent our partitions.
    //

    IOMedia *    partition;
    OSIterator * partitionIterator;

    // State our assumptions.

    assert(_partitions);

    // Ask our superclass' opinion.

    if ( super::start(provider) == false )  return false;

    // Attach and register the new media objects representing our partitions.

    partitionIterator = OSCollectionIterator::withCollection(_partitions);
    if ( partitionIterator == 0 )  return false;

    while ( (partition = (IOMedia *) partitionIterator->getNextObject()) )
    {
        if ( partition->attach(this) )
        {
            partition->registerService();
        }
    }

    partitionIterator->release();

    return true;
}

OSSet * IOCDPartitionScheme::scan(SInt32 * score)
{
    //
    // Scan the provider media for CD partitions (in TOC).  Returns the set
    // of media objects representing each of the partitions (the retain for
    // the set is passed to the caller), or null should no CD partitions be
    // found.  The default probe score can be adjusted up or down, based on
    // the confidence of the scan.
    //

    struct CDSession
    {
        UInt32 formed:1;
        UInt32 leadOut;
    };

    struct CDTrack
    {
        UInt32            block;
        CDSectorSize      blockSize;
        CDSectorType      blockType;
        CDTOCDescriptor * descriptor;
        UInt32            session:8;
    };

    #define kCDSessionMaxIndex 0x63

    #define kCDTrackMinIndex   0x01
    #define kCDTrackMaxIndex   0x63

    IOBufferMemoryDescriptor * buffer          = 0;
    IOCDMedia *                media           = getProvider();
    UInt64                     mediaBlockSize  = media->getPreferredBlockSize();
    bool                       mediaIsOpen     = false;
    OSSet *                    partitions      = 0;
    CDSession *                sessions        = 0;
    UInt32                     sessionMinIndex = kCDSessionMaxIndex + 1;
    UInt32                     sessionMaxIndex = 0;
    CDTOC *                    toc             = 0;
    UInt32                     tocCount        = 0;
    CDTrack *                  tracks          = 0;
    UInt32                     trackMinIndex   = kCDTrackMaxIndex + 1;
    UInt32                     trackMaxIndex   = 0;
    CDTrack *                  trackMaxLinked  = 0;

    // State our assumptions.

    assert(mediaBlockSize == kCDSectorSizeWhole);

    // Determine whether this media is formatted.

    if ( media->isFormatted() == false )  goto scanErr;

    // Allocate a buffer large enough to hold a whole 2352-byte sector.

    buffer = IOBufferMemoryDescriptor::withCapacity(
                                           /* capacity      */ mediaBlockSize,
                                           /* withDirection */ kIODirectionIn );
    if ( buffer == 0 )  goto scanErr;

    // Allocate a set to hold the set of media objects representing partitions.

    partitions = OSSet::withCapacity(2);
    if ( partitions == 0 )  goto scanErr;

    // Open the media with read access.

    mediaIsOpen = media->open(this, 0, kIOStorageAccessReader);
    if ( mediaIsOpen == false )  goto scanErr;

    // Obtain the table of contents.

    toc = media->getTOC();
    if ( toc )  tocCount = CDTOCGetDescriptorCount(toc);

    // Allocate a list large enough to hold information about each session.

    sessions = IONew(CDSession, kCDSessionMaxIndex + 1);
    if ( sessions == 0 )  goto scanErr;

    bzero(sessions, (kCDSessionMaxIndex + 1) * sizeof(CDSession));

    // Allocate a list large enough to hold information about each track.

    tracks = IONew(CDTrack, kCDTrackMaxIndex + 1);
    if ( tracks == 0 )  goto scanErr;

    bzero(tracks, (kCDTrackMaxIndex + 1) * sizeof(CDTrack));

    // Scan the table of contents, gathering information about the sessions
    // and tracks on the CD, but without making assumptions about the order
    // of the entries in the table.

    for ( unsigned index = 0; index < tocCount; index++ )
    {
        CDTOCDescriptor * descriptor = toc->descriptors + index;

        // Determine whether this is an audio or data track descriptor.

        if ( descriptor->point   >= kCDTrackMinIndex   &&
             descriptor->point   <= kCDTrackMaxIndex   &&
             descriptor->adr     == 0x01               &&
             descriptor->session <= kCDSessionMaxIndex )
        {
            CDTrack * track = tracks + descriptor->point;

            // Record the relevant information about this track.

            track->block      = CDConvertMSFToClippedLBA(descriptor->p);
            track->descriptor = descriptor;
            track->session    = descriptor->session;

            if ( (descriptor->control & 0x04) )                 // (data track?)
            {
                track->blockSize = kCDSectorSizeMode1;
                track->blockType = kCDSectorTypeMode1;
            }
            else                                               // (audio track?)
            {
                track->blockSize = kCDSectorSizeCDDA;
                track->blockType = kCDSectorTypeCDDA;
            }

            trackMinIndex = min(descriptor->point, trackMinIndex);
            trackMaxIndex = max(descriptor->point, trackMaxIndex);
        }

        // Determine whether this is a lead-in (A0) descriptor.

        else if ( descriptor->point   == 0xA0 &&
                  descriptor->adr     == 0x01 &&
                  descriptor->session <= kCDSessionMaxIndex )
        {
            CDSession * session = sessions + descriptor->session;

            // Record whether the session has "form 1" or "form 2" tracks.

            session->formed = ( descriptor->p.second ) ? true : false;
        }

        // Determine whether this is a lead-out (A2) descriptor.

        else if ( descriptor->point   == 0xA2               &&
                  descriptor->adr     == 0x01               &&
                  descriptor->session <= kCDSessionMaxIndex )
        {
            CDSession * session = sessions + descriptor->session;

            // Record the position of the session lead-out.

            session->leadOut = CDConvertMSFToClippedLBA(descriptor->p);

            sessionMinIndex = min(descriptor->session, sessionMinIndex);
            sessionMaxIndex = max(descriptor->session, sessionMaxIndex);
        }
    }

    // Pre-scan the ordered list of tracks.

    for ( unsigned index = trackMinIndex; index <= trackMaxIndex; index++ )
    {
        CDTrack * track = tracks + index;

        // Validate the existence of this track (and its session).

        if ( track->descriptor == 0 || sessions[track->session].leadOut == 0 )
        {
            goto scanErr;
        }

        // Determine the block type, and linkage requirement, for this track.

        if ( track->blockType == kCDSectorTypeMode1 )           // (data track?)
        {
            IOReturn status;

            // Read a whole sector from the data track into our buffer.

            status = media->read( /* client    */ this,
                                  /* byteStart */ track->block * mediaBlockSize,
                                  /* buffer    */ buffer );

            if ( status == kIOReturnSuccess )
            {
                UInt8 * sector = (UInt8 *) buffer->getBytesNoCopy();

                // Determine whether this is a "mode 2" data track.

                if ( sector[15] == 0x02 )
                {
                    // Determine whether this is a "mode 2 formless",
                    // "mode 2 form 1" or "mode 2 form 2" data track.

                    if ( sessions[track->session ].formed ||
                         sessions[sessionMinIndex].formed )
                    {
                        if ( (sector[18] & 0x20) )
                        {
                            track->blockSize = kCDSectorSizeMode2Form2;
                            track->blockType = kCDSectorTypeMode2Form2;

                            trackMaxLinked = track;
                        }
                        else
                        {
                            track->blockSize = kCDSectorSizeMode2Form1;
                            track->blockType = kCDSectorTypeMode2Form1;

                            // Determine whether this is a linked data track.

                            if ( memcmp(sector + 24, "ER", 2) )
                            {
                                trackMaxLinked = track;
                            }
                        }
                    }
                    else
                    {
                        track->blockSize = kCDSectorSizeMode2;
                        track->blockType = kCDSectorTypeMode2;

                        trackMaxLinked = track;
                    }
                }

                // Determine whether this is a linked "mode 1" data track.

                else if ( memcmp(sector + 16, "ER", 2) )
                {
                    trackMaxLinked = track;
                }
            }
            else
            {
                trackMaxLinked = track;
            }
        }
    }

    // Create a media object to represent the linked data tracks, the hidden
    // pre-gap-area data track, or even both, if it is applicable to this CD.

    if ( trackMinIndex > kCDTrackMaxIndex ||                     // (no tracks?)
         trackMaxLinked                   ||                 // (linked tracks?)
         tracks[trackMinIndex].block      )                  // (hidden tracks?)
    {
        CDTOCDescriptor descriptor;
        UInt32          trackBlockNext;
        CDSectorSize    trackBlockSize;
        CDSectorType    trackBlockType;
        UInt64          trackSize;

        descriptor.session        = 0x01;
        descriptor.control        = 0x04;
        descriptor.adr            = 0x01;
        descriptor.tno            = 0x00;
        descriptor.point          = 0x00;
        descriptor.address.minute = 0x00;
        descriptor.address.second = 0x00;
        descriptor.address.frame  = 0x00;
        descriptor.zero           = 0x00;
        descriptor.p              = CDConvertLBAToMSF(0);

        if ( trackMinIndex > kCDTrackMaxIndex )                  // (no tracks?)
        {
            if ( sessionMinIndex > kCDSessionMaxIndex )        // (no sessions?)
            {
                if ( media->isWritable() == false )  goto scanErr;
            }
            else
            {
                descriptor.session = sessionMaxIndex;
            }

            if ( media->isWritable() )                   // (is still writable?)
            {
                CDPMA * pma      = (CDPMA *) buffer->getBytesNoCopy();
                UInt32  pmaCount = 0;
                UInt16  pmaSize  = 0;

                trackBlockNext = media->getSize() / mediaBlockSize;
                trackBlockSize = kCDSectorSizeMode2Form1;
                trackBlockType = kCDSectorTypeMode2Form1;

                // Determine whether this is a "mode 1" data track.

                media->readTOC( /* buffer               */ buffer,
                                /* format               */ kCDTOCFormatPMA,
                                /* formatAsTime         */ true,
                                /* trackOrSessionNumber */ 0,
                                /* actualByteCount      */ &pmaSize );

                pmaSize  = ( pmaSize <= sizeof(CDPMA) )
                               ? pmaSize
                               : min( pmaSize, 
                                      OSSwapBigToHostInt16(pma->dataLength) +
                                                    sizeof(pma->dataLength) );
                pmaCount = ( pmaSize <= sizeof(CDPMA) )
                               ? 0
                               : ( pmaSize - sizeof(CDPMA) ) / 
                                             sizeof(CDPMADescriptor);

                for ( unsigned index = 0; index < pmaCount; index++ )
                {
                    if ( pma->descriptors[index].adr == 0x2 )
                    {
                        if ( pma->descriptors[index].p.second == 0x00 )
                        {
                            trackBlockSize = kCDSectorSizeMode1;
                            trackBlockType = kCDSectorTypeMode1;
                            break;
                        }
                    }
                }
            }
            else
            {
                trackBlockNext = sessions[sessionMaxIndex].leadOut;
                trackBlockSize = kCDSectorSizeMode2Form1;
                trackBlockType = kCDSectorTypeMode2Form1;
            }
        }
        else if ( trackMaxLinked )                           // (linked tracks?)
        {
            descriptor.session = sessionMaxIndex;
            descriptor.control = trackMaxLinked->descriptor->control;

            if ( media->isWritable() )                   // (is still writable?)
            {
                trackBlockNext = media->getSize() / mediaBlockSize;
            }
            else
            {
                trackBlockNext = sessions[sessionMaxIndex].leadOut;
            }

            if ( trackMaxLinked->blockType == kCDSectorTypeMode1 )
            {
                trackBlockSize = kCDSectorSizeMode1;
                trackBlockType = kCDSectorTypeMode1;
            }
            else
            {
                trackBlockSize = kCDSectorSizeMode2Form1;
                trackBlockType = kCDSectorTypeMode2Form1;
            }
        }
        else                                                 // (hidden tracks?)
        {
            IOReturn status;

            descriptor.session = sessionMinIndex;

            trackBlockNext = tracks[trackMinIndex].block;
            trackBlockSize = kCDSectorSizeMode1;
            trackBlockType = kCDSectorTypeMode1;

            // Read a whole sector from the hidden track into our buffer.

            status = media->read( /* client    */ this,
                                  /* byteStart */ 0,
                                  /* buffer    */ buffer );

            if ( status == kIOReturnSuccess )
            {
                UInt8 * sector = (UInt8 *) buffer->getBytesNoCopy();

                // Determine whether this is an audio track.

                if ( sector[ 0] != 0x00 ||
                     sector[ 1] != 0xFF ||
                     sector[ 2] != 0xFF ||
                     sector[ 3] != 0xFF ||
                     sector[ 4] != 0xFF ||
                     sector[ 5] != 0xFF ||
                     sector[ 6] != 0xFF ||
                     sector[ 7] != 0xFF ||
                     sector[ 8] != 0xFF ||
                     sector[ 9] != 0xFF ||
                     sector[10] != 0xFF ||
                     sector[11] != 0x00 )
                {
                    trackBlockSize = kCDSectorSizeCDDA;
                    trackBlockType = kCDSectorTypeCDDA;
                }
            }
        }

        trackSize = trackBlockNext * trackBlockSize;

        // Create a media object to represent this partition.

        IOMedia * newMedia = instantiateMediaObject(
                                 /* partition          */ &descriptor,
                                 /* partitionSize      */ trackSize,
                                 /* partitionBlockSize */ trackBlockSize,
                                 /* partitionBlockType */ trackBlockType,
                                 /* toc                */ toc );

        if ( newMedia )
        {
            partitions->setObject(newMedia);
            newMedia->release();
        }
    }

    // Scan the ordered list of tracks.

    for ( unsigned index = trackMinIndex; index <= trackMaxIndex; index++ )
    {
        CDTrack * track = tracks + index;
        UInt32    trackBlockNext;
        UInt64    trackSize;

        // Determine whether this is an audio track or an unlinked data track.

        if ( ( ( track->blockType != kCDSectorTypeCDDA       ) ) &&
             ( ( track->blockType != kCDSectorTypeMode1      &&
                 track->blockType != kCDSectorTypeMode2Form1 ) ||
               ( trackMaxLinked                              ) ) )
        {
            continue;
        }

        // Determine where the partitions ends.

        if ( index < trackMaxIndex && track->session == (track + 1)->session )
        {
            trackBlockNext = (track + 1)->block;
        }
        else
        {
            trackBlockNext = sessions[track->session].leadOut;
        }

        if ( track->block >= trackBlockNext )
        {
            goto scanErr;
        }

        trackSize = (trackBlockNext - track->block) * track->blockSize;

        // Determine whether the partition is corrupt (fatal).

        if ( isPartitionCorrupt( /* partition          */ track->descriptor,
                                 /* partitionSize      */ trackSize,
                                 /* partitionBlockSize */ track->blockSize,
                                 /* partitionBlockType */ track->blockType,
                                 /* toc                */ toc ) )
        {
            goto scanErr;
        }

        // Determine whether the partition is invalid (skipped).

        if ( isPartitionInvalid( /* partition          */ track->descriptor,
                                 /* partitionSize      */ trackSize,
                                 /* partitionBlockSize */ track->blockSize,
                                 /* partitionBlockType */ track->blockType,
                                 /* toc                */ toc ) )
        {
            continue;
        }

        // Create a media object to represent this partition.

        IOMedia * newMedia = instantiateMediaObject(
                                 /* partition          */ track->descriptor,
                                 /* partitionSize      */ trackSize,
                                 /* partitionBlockSize */ track->blockSize,
                                 /* partitionBlockType */ track->blockType,
                                 /* toc                */ toc );

        if ( newMedia )
        {
            partitions->setObject(newMedia);
            newMedia->release();
        }
    }

    // Release our resources.

    media->close(this);
    buffer->release();
    IODelete(tracks, CDTrack, kCDTrackMaxIndex + 1);
    IODelete(sessions, CDSession, kCDSessionMaxIndex + 1);

    return partitions;

scanErr:

    // Release our resources.

    if ( mediaIsOpen )  media->close(this);
    if ( partitions )  partitions->release();
    if ( buffer )  buffer->release();
    if ( tracks )  IODelete(tracks, CDTrack, kCDTrackMaxIndex + 1);
    if ( sessions )  IODelete(sessions, CDSession, kCDSessionMaxIndex + 1);

    return 0;
}

bool IOCDPartitionScheme::isPartitionCorrupt(
                                    CDTOCDescriptor * /* partition          */ ,
                                    UInt64            /* partitionSize      */ ,
                                    UInt32            /* partitionBlockSize */ ,
                                    CDSectorType      /* partitionBlockType */ ,
                                    CDTOC *           /* toc                */ )
{
    //
    // Ask whether the given partition appears to be corrupt.  A partition that
    // is corrupt will cause the failure of the CD partition scheme altogether.
    //

    return false;
}

bool IOCDPartitionScheme::isPartitionInvalid(
                                    CDTOCDescriptor * partition,
                                    UInt64            partitionSize,
                                    UInt32            partitionBlockSize,
                                    CDSectorType      partitionBlockType,
                                    CDTOC *           toc )
{
    //
    // Ask whether the given partition appears to be invalid.  A partition that
    // is invalid will cause it to be skipped in the scan, but will not cause a
    // failure of the CD partition scheme.
    //

    IOMedia * media          = getProvider();
    UInt64    mediaBlockSize = media->getPreferredBlockSize();
    UInt64    partitionBase  = 0;

    // Compute the relative byte position and size of the new partition,
    // relative to the provider media's natural blocking factor of 2352.

    partitionBase = CDConvertMSFToClippedLBA(partition->p) * mediaBlockSize;
    partitionSize = (partitionSize / partitionBlockSize) * mediaBlockSize;

    // Determine whether the partition leaves the confines of the container.

    if ( partitionBase + partitionSize > media->getSize() )  return true;

    return false;
}

IOMedia * IOCDPartitionScheme::instantiateMediaObject(
                                    CDTOCDescriptor * partition,
                                    UInt64            partitionSize,
                                    UInt32            partitionBlockSize,
                                    CDSectorType      partitionBlockType,
                                    CDTOC *           toc )
{
    //
    // Instantiate a new media object to represent the given partition.
    //

    IOMedia * media               = getProvider();
    UInt64    partitionBase       = 0;
    char *    partitionHint       = 0;
    bool      partitionIsWritable = media->isWritable();

    // Compute the relative byte position of the new partition and encode it
    // into the designated "logical space", given the partition's block type.
    //
    // 0x0000000000 through 0x00FFFFFFFF is the "don't care" space.
    // 0x0100000000 through 0x01FFFFFFFF is the "audio" space.
    // 0x0200000000 through 0x02FFFFFFFF is the "mode 1" space.
    // 0x0300000000 through 0x03FFFFFFFF is the "mode 2 formless" space.
    // 0x0400000000 through 0x04FFFFFFFF is the "mode 2 form 1" space.
    // 0x0500000000 through 0x05FFFFFFFF is the "mode 2 form 2" space.

    partitionBase  = CDConvertMSFToClippedLBA(partition->p);
    partitionBase *= partitionBlockSize;
    partitionBase += ((UInt64) partitionBlockType) << 32;

    // Look up a type for the new partition.

    OSDictionary * hintTable = OSDynamicCast( 
              /* type     */ OSDictionary,
              /* instance */ getProperty(kIOCDPartitionSchemeContentTable) );

    if ( hintTable )
    {
        char       hintIndex[5];
        OSString * hintValue;

        snprintf(hintIndex, sizeof(hintIndex), "0x%02X", partitionBlockType & 0xFF);

        hintValue = OSDynamicCast(OSString, hintTable->getObject(hintIndex));

        if ( hintValue ) partitionHint = (char *) hintValue->getCStringNoCopy();
    }

    // Determine whether the new partition is read-only.

    if ( partition->point )  partitionIsWritable = false;

    // Create the new media object.

    IOMedia * newMedia = instantiateDesiredMediaObject(
                                 /* partition          */ partition,
                                 /* partitionSize      */ partitionSize,
                                 /* partitionBlockSize */ partitionBlockSize,
                                 /* partitionBlockType */ partitionBlockType,
                                 /* toc                */ toc );

    if ( newMedia )
    {
        if ( newMedia->init(
                /* base               */ partitionBase,
                /* size               */ partitionSize,
                /* preferredBlockSize */ partitionBlockSize,
                /* attributes         */ media->getAttributes(),
                /* isWhole            */ false,
                /* isWritable         */ partitionIsWritable,
                /* contentHint        */ partitionHint ) )
        {
            // Set a name for this partition.

            char name[24];
            snprintf(name, sizeof(name), "Untitled %d", partition->point);
            newMedia->setName(name);

            // Set a location value (the partition number) for this partition.

            char location[12];
            snprintf(location, sizeof(location), "%d", partition->point);
            newMedia->setLocation(location);

            // Set the "Partition ID" key for this partition.

            newMedia->setProperty(kIOMediaPartitionIDKey, partition->point, 32);

            // Set the "Session ID" key for this partition.

            newMedia->setProperty(kIOMediaSessionIDKey, partition->session, 32);
        }
        else
        {
            newMedia->release();
            newMedia = 0;
        }
    }

    return newMedia;
}

IOMedia * IOCDPartitionScheme::instantiateDesiredMediaObject(
                                    CDTOCDescriptor * /* partition          */ ,
                                    UInt64            /* partitionSize      */ ,
                                    UInt32            /* partitionBlockSize */ ,
                                    CDSectorType      /* partitionBlockType */ ,
                                    CDTOC *           /* toc                */ )
{
    //
    // Allocate a new media object (called from instantiateMediaObject).
    //

    return new IOMedia;
}

void IOCDPartitionScheme::read( IOService *           client,
                                UInt64                byteStart,
                                IOMemoryDescriptor *  buffer,
                                IOStorageAttributes * attributes,
                                IOStorageCompletion * completion )
{
    //
    // Read data from the storage object at the specified byte offset into the
    // specified buffer, asynchronously.   When the read completes, the caller
    // will be notified via the specified completion action.
    //
    // The buffer will be retained for the duration of the read.
    //
    // For the CD partition scheme, we convert the read from a partition
    // object into the appropriate readCD command to our provider media.
    //

    getProvider()->readCD( /* client     */ this,
                           /* byteStart  */ (byteStart & 0xFFFFFFFF),
                           /* buffer     */ buffer,
                           /* sectorArea */ (CDSectorArea) kCDSectorAreaUser,
                           /* sectorType */ (CDSectorType) (byteStart >> 32),
#ifdef __LP64__
                           /* attributes */ attributes,
                           /* completion */ completion );
#else /* !__LP64__ */
                           /* completion */ completion ? *completion : (IOStorageCompletion) { 0 } );
#endif /* !__LP64__ */
}

void IOCDPartitionScheme::write( IOService *           client,
                                 UInt64                byteStart,
                                 IOMemoryDescriptor *  buffer,
                                 IOStorageAttributes * attributes,
                                 IOStorageCompletion * completion )
{
    //
    // Write data into the storage object at the specified byte offset from the
    // specified buffer, asynchronously.   When the write completes, the caller
    // will be notified via the specified completion action.
    //
    // The buffer will be retained for the duration of the write.
    //
    // For the CD partition scheme, we convert the write from a partition
    // object into the appropriate writeCD command to our provider media.
    //

    getProvider()->writeCD( /* client     */ this,
                            /* byteStart  */ (byteStart & 0xFFFFFFFF),
                            /* buffer     */ buffer,
                            /* sectorArea */ (CDSectorArea) kCDSectorAreaUser,
                            /* sectorType */ (CDSectorType) (byteStart >> 32),
#ifdef __LP64__
                            /* attributes */ attributes,
                            /* completion */ completion );
#else /* !__LP64__ */
                            /* completion */ completion ? *completion : (IOStorageCompletion) { 0 } );
#endif /* !__LP64__ */
}

OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  0);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  1);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  2);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  3);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  4);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  5);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  6);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  7);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  8);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme,  9);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme, 10);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme, 11);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme, 12);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme, 13);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme, 14);
OSMetaClassDefineReservedUnused(IOCDPartitionScheme, 15);
