/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2020 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "getkeys.h"

#include "ep_engine.h"
#include "kv_bucket.h"

#include <mcbp/protocol/datatype.h>
#include <mcbp/protocol/status.h>
#include <phosphor/phosphor.h>

void AllKeysCallback::callback(const DiskDocKey& key) {
    auto outKey = key.getDocKey();
    if (outKey.isPrivate() || key.isPrepared()) {
        // Skip system-event and durability-prepared keys
        return;
    }

    if (!collectionsSupported) {
        if (outKey.getCollectionID().isDefaultCollection()) {
            outKey = outKey.makeDocKeyWithoutCollectionID();
        } else {
            // Only default collection key can be sent back if
            // collectionsSupported is false
            return;
        }
    }

    if (buffer.size() + outKey.size() + sizeof(uint16_t) > buffer.size()) {
        // Reserve the 2x space for the copy-to buffer.
        buffer.reserve(buffer.size() * 2);
    }
    uint16_t outlen = htons(outKey.size());
    // insert 1 x u16
    const auto* outlenPtr = reinterpret_cast<const char*>(&outlen);
    buffer.insert(buffer.end(), outlenPtr, outlenPtr + sizeof(uint16_t));
    // insert the char buffer
    buffer.insert(buffer.end(), outKey.data(), outKey.data() + outKey.size());
}

FetchAllKeysTask::FetchAllKeysTask(EventuallyPersistentEngine* e,
                                   const void* c,
                                   AddResponseFn resp,
                                   const DocKey start_key_,
                                   Vbid vbucket,
                                   uint32_t count_,
                                   bool collectionsSupported)
    : GlobalTask(e, TaskId::FetchAllKeysTask, 0, false),
      cookie(c),
      description("Running the ALL_DOCS api on " + vbucket.to_string()),
      response(std::move(resp)),
      start_key(start_key_),
      vbid(vbucket),
      count(count_),
      collectionsSupported(collectionsSupported) {
}

bool FetchAllKeysTask::run() {
    TRACE_EVENT0("ep-engine/task", "FetchAllKeysTask");
    ENGINE_ERROR_CODE err;
    if (engine->getKVBucket()
                ->getVBuckets()
                .getBucket(vbid)
                ->isBucketCreation()) {
        // Returning an empty packet with a SUCCESS response as
        // there aren't any keys during the vbucket file creation.
        err = response({}, // key
                       {}, // extra
                       {}, // body
                       PROTOCOL_BINARY_RAW_BYTES,
                       cb::mcbp::Status::Success,
                       0,
                       cookie)
                      ? ENGINE_SUCCESS
                      : ENGINE_FAILED;
    } else {
        auto cb = std::make_shared<AllKeysCallback>(collectionsSupported);
        err = engine->getKVBucket()->getROUnderlying(vbid)->getAllKeys(
                vbid, start_key, count, cb);
        if (err == ENGINE_SUCCESS) {
            err = response({}, // key
                           {}, // extra
                           {((AllKeysCallback*)cb.get())->getAllKeysPtr(),
                            ((AllKeysCallback*)cb.get())->getAllKeysLen()},
                           PROTOCOL_BINARY_RAW_BYTES,
                           cb::mcbp::Status::Success,
                           0,
                           cookie)
                          ? ENGINE_SUCCESS
                          : ENGINE_FAILED;
        }
    }
    engine->addLookupAllKeys(cookie, err);
    engine->notifyIOComplete(cookie, err);
    return false;
}
