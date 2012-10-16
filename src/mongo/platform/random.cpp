// random.cpp

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/random.h"

#include <cstdlib>

#include "mongo/platform/basic.h"

namespace mongo {

#ifdef _WIN32
#pragma warning( disable : 4715 ) // not all control paths return a value
    int32_t PseudoRandom::nextInt32() {
        if ( rand_s(&_seed) == 0 ) {
            // SUCCESS
            return _seed;
        }
        abort();
    }

#else
    int32_t PseudoRandom::nextInt32() {
        return rand_r( &_seed );
    }
#endif

}
