#pragma once
#include <string>
#include <vector>

#include "Utility/Export.h"

namespace FastNoise
{
    /** @brief Reading node trees encoded by FastNoise2 before 1.0.
     *
     *  Before 1.0 an encoded node tree used 16-bit node ids, a different node id
     *  order, and wrote every member of every node. Those strings do not decode as
     *  the current format, and nothing in them marks which format they are: a
     *  caller that stored them must know that they are legacy.
     *
     *  This is an Infinity fork addition. It exists so projects saved against the
     *  pre-1.0 library keep loading, and is meant to be removed once they have
     *  been re-saved.
     */
    namespace Legacy
    {
        /** @brief Convert a pre-1.0 encoded node tree to the current encoding.
         *
         *  Each legacy node becomes the current node that does the same thing,
         *  with its parameters carried over. Where the current node changed its
         *  maths, the tree is rewritten so the output matches the legacy one:
         *
         *  - Fractals no longer divide by their bounding factor, so a Multiply
         *    node applies it.
         *  - FractalPingPong was removed. Its per-octave function was a sawtooth,
         *    not the ping-pong 1.0 computes, so it is rebuilt from Add, Multiply,
         *    Terrace and Subtract nodes under an FBm fractal.
         *  - Coherent noise nodes gained a Feature Scale, set to 1 so they sample
         *    the same coordinates as before.
         *  - CellularLookup lost its lookup frequency, so a DomainScale node
         *    applies it to the lookup source.
         *  - A Euclidean CellularDistance returned the square root of each
         *    distance; SignedSquareRoot nodes restore that.
         *  - A domain warp's frequency is carried as its Feature Scale, and the
         *    fractal bounding of a domain warp fractal as its amplitude.
         *  - Checkerboard, CellularDistance and Fade use output or input ranges
         *    that reproduce the legacy values.
         *
         *  The output is not identical: the node hashes changed in 1.0, so every
         *  hashed node (white, value, gradient, simplex and cellular noise, and
         *  domain warps) produces a different but statistically equivalent
         *  field. @p notes lists the nodes whose behaviour cannot be reproduced
         *  exactly, once per kind of difference.
         *
         *  @param legacyEncodedNodeTree  String written by the pre-1.0 SerialiseNodeData.
         *  @param notes                  Optional. Receives a line per approximation made.
         *  @return The tree in the current encoding, or an empty string if the input
         *          is not a complete, valid legacy tree (trailing bytes included).
         */
        FASTNOISE_API std::string ConvertEncodedNodeTree( const char* legacyEncodedNodeTree, std::vector<std::string>* notes = nullptr );

        /** @brief Whether a string of unknown origin is a pre-1.0 encoded node tree.
         *
         *  For callers that cannot know which format a string is in. The formats
         *  overlap: the current decoder accepts about half of all pre-1.0 strings
         *  as some other tree, and a few current strings parse as complete pre-1.0
         *  trees. This returns true only for a complete pre-1.0 tree that is not
         *  also a current one, which is decided by two facts about the current
         *  writer: its Base64 contains '@' (a run of 'A's) where the old one never
         *  did, and its output is canonical, so a string it wrote decodes and
         *  encodes back to itself.
         *
         *  A caller that knows the format should not use this: a current string
         *  that is also a valid pre-1.0 tree can still be misjudged.
         */
        FASTNOISE_API bool IsEncodedNodeTree( const char* encodedNodeTree );
    }
}
