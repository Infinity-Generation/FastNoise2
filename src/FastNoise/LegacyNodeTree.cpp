// Reads node trees encoded by FastNoise2 before 1.0 and rewrites them in the
// current encoding. Infinity fork addition; see LegacyNodeTree.h.
//
// The legacy layout below is FROZEN: it is the pre-1.0 node list and member
// order, not something derived from the current metadata. Every value in a
// legacy stream is positional, so one wrong count misreads the rest of the tree.

#include "FastNoise/LegacyNodeTree.h"
#include "FastNoise/Metadata.h"
#include "Base64.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace FastNoise;

namespace
{
    // Pre-1.0 node ids, in the order that library registered them.
    enum LegacyId : uint16_t
    {
        kConstant, kWhite, kCheckerboard, kSineWave, kPositionOutput, kDistanceToPoint,
        kValue, kPerlin, kSimplex, kOpenSimplex2,
        kCellularValue, kCellularDistance, kCellularLookup,
        kFractalFBm, kFractalPingPong, kFractalRidged,
        kDomainWarpGradient, kDomainWarpFractalProgressive, kDomainWarpFractalIndependant,
        kDomainScale, kDomainOffset, kDomainRotate, kSeedOffset, kRemap, kConvertRGBA8,
        kAdd, kSubtract, kMultiply, kDivide, kMin, kMax, kMinSmooth, kMaxSmooth, kFade,
        kTerrace, kPowFloat, kPowInt, kDomainAxisScale, kAddDimension, kRemoveDimension, kGeneratorCache,
        kOpenSimplex2S,
        kLegacyNodeCount
    };

    // Member counts per legacy node, in stream order: variables (4 bytes each),
    // then node lookups, then hybrids (a flag byte, then 4 bytes or a node).
    struct LegacyLayout
    {
        uint8_t variables;
        uint8_t lookups;
        uint8_t hybrids;
    };

    constexpr LegacyLayout kLegacyLayouts[kLegacyNodeCount] =
    {
        { 1, 0, 0 }, // Constant: Value
        { 0, 0, 0 }, // White
        { 1, 0, 0 }, // Checkerboard: Size
        { 1, 0, 0 }, // SineWave: Scale
        { 8, 0, 0 }, // PositionOutput: Multiplier XYZW, Offset XYZW
        { 5, 0, 0 }, // DistanceToPoint: Distance Function, Point XYZW
        { 0, 0, 0 }, // Value
        { 0, 0, 0 }, // Perlin
        { 0, 0, 0 }, // Simplex
        { 0, 0, 0 }, // OpenSimplex2
        { 2, 0, 1 }, // CellularValue: Distance Function, Value Index | Jitter Modifier
        { 4, 0, 1 }, // CellularDistance: Distance Function, Distance Index 0, Distance Index 1, Return Type | Jitter Modifier
        { 2, 1, 1 }, // CellularLookup: Distance Function, Lookup Frequency | Lookup | Jitter Modifier
        { 2, 1, 2 }, // FractalFBm: Octaves, Lacunarity | Source | Gain, Weighted Strength
        { 2, 1, 3 }, // FractalPingPong: Octaves, Lacunarity | Source | Gain, Weighted Strength, Ping Pong Strength
        { 2, 1, 2 }, // FractalRidged: Octaves, Lacunarity | Source | Gain, Weighted Strength
        { 1, 1, 1 }, // DomainWarpGradient: Warp Frequency | Source | Warp Amplitude
        { 2, 1, 2 }, // DomainWarpFractalProgressive: Octaves, Lacunarity | Domain Warp Source | Gain, Weighted Strength
        { 2, 1, 2 }, // DomainWarpFractalIndependant: same
        { 1, 1, 0 }, // DomainScale: Scale | Source
        { 0, 1, 4 }, // DomainOffset: Source | Offset XYZW
        { 3, 1, 0 }, // DomainRotate: Yaw, Pitch, Roll | Source
        { 1, 1, 0 }, // SeedOffset: Seed Offset | Source
        { 4, 1, 0 }, // Remap: From Min, From Max, To Min, To Max | Source
        { 2, 1, 0 }, // ConvertRGBA8: Min, Max | Source
        { 0, 1, 1 }, // Add: LHS | RHS
        { 0, 0, 2 }, // Subtract: LHS, RHS
        { 0, 1, 1 }, // Multiply: LHS | RHS
        { 0, 0, 2 }, // Divide: LHS, RHS
        { 0, 1, 1 }, // Min: LHS | RHS
        { 0, 1, 1 }, // Max: LHS | RHS
        { 0, 1, 2 }, // MinSmooth: LHS | RHS, Smoothness
        { 0, 1, 2 }, // MaxSmooth: LHS | RHS, Smoothness
        { 0, 2, 1 }, // Fade: A, B | Fade
        { 2, 1, 0 }, // Terrace: Multiplier, Smoothness | Source
        { 0, 0, 2 }, // PowFloat: Value, Pow
        { 1, 1, 0 }, // PowInt: Pow | Value
        { 4, 1, 0 }, // DomainAxisScale: Scale XYZW | Source
        { 0, 1, 1 }, // AddDimension: Source | New Dimension Position
        { 1, 1, 0 }, // RemoveDimension: Remove Dimension | Source
        { 0, 1, 0 }, // GeneratorCache: Source
        { 0, 0, 0 }, // OpenSimplex2S
    };

    // The pre-1.0 Cellular jitter for 2D sampling; 1.0 kept the same constant.
    constexpr float kCellularJitter2D = 0.437016f;

    // Enum values shared by both formats.
    constexpr int kEuclidean = 0;                               // DistanceFunction::Euclidean
    enum { kIndex0, kIndex0Add1, kIndex0Sub1, kIndex0Mul1, kIndex0Div1 }; // CellularDistance::ReturnType

    // A legacy tree nests one node per level; real trees are a handful deep.
    constexpr int kMaxDepth = 256;

    struct LegacyNode
    {
        struct Hybrid
        {
            const LegacyNode* node = nullptr;
            float value = 0.0f;
        };

        uint16_t id = 0;
        std::vector<int32_t> variables;
        std::vector<const LegacyNode*> lookups;
        std::vector<Hybrid> hybrids;

        float Float( size_t i ) const
        {
            float f;
            std::memcpy( &f, &variables[i], sizeof( f ) );
            return f;
        }

        int Int( size_t i ) const { return variables[i]; }
    };

    struct ConversionError {};

    class LegacyReader
    {
    public:
        explicit LegacyReader( std::vector<uint8_t> data ) : mData( std::move( data ) ) {}

        const LegacyNode* ReadTree()
        {
            const LegacyNode* root = ReadNode( 0 );

            // The pre-1.0 writer produced exactly one tree and nothing after it.
            if( mPos != mData.size() )
            {
                throw ConversionError{};
            }
            return root;
        }

    private:
        template<typename T>
        T Read()
        {
            if( mData.size() - mPos < sizeof( T ) )
            {
                throw ConversionError{};
            }
            T value;
            std::memcpy( &value, mData.data() + mPos, sizeof( T ) );
            mPos += sizeof( T );
            return value;
        }

        const LegacyNode* ReadNode( int depth )
        {
            if( depth > kMaxDepth )
            {
                throw ConversionError{};
            }

            uint16_t id = Read<uint16_t>();

            // UINT16_MAX marks a reference to a node already decoded, by the index
            // in which nodes were completed (children before their parent).
            if( id == std::numeric_limits<uint16_t>::max() )
            {
                uint16_t reference = Read<uint16_t>();
                if( reference >= mCompleted.size() )
                {
                    throw ConversionError{};
                }
                return mCompleted[reference];
            }

            if( id >= kLegacyNodeCount )
            {
                throw ConversionError{};
            }

            const LegacyLayout& layout = kLegacyLayouts[id];
            mNodes.emplace_back( new LegacyNode );
            LegacyNode* node = mNodes.back().get();
            node->id = id;

            for( uint8_t i = 0; i < layout.variables; i++ )
            {
                node->variables.push_back( Read<int32_t>() );
            }

            for( uint8_t i = 0; i < layout.lookups; i++ )
            {
                node->lookups.push_back( ReadNode( depth + 1 ) );
            }

            for( uint8_t i = 0; i < layout.hybrids; i++ )
            {
                LegacyNode::Hybrid hybrid;
                uint8_t isNode = Read<uint8_t>();

                if( isNode == 1 )
                {
                    hybrid.node = ReadNode( depth + 1 );
                }
                else if( isNode == 0 )
                {
                    hybrid.value = Read<float>();
                }
                else
                {
                    throw ConversionError{};
                }
                node->hybrids.push_back( hybrid );
            }

            mCompleted.push_back( node );
            return node;
        }

        std::vector<uint8_t> mData;
        size_t mPos = 0;
        std::vector<std::unique_ptr<LegacyNode>> mNodes;
        std::vector<const LegacyNode*> mCompleted;
    };

    const Metadata& FindMetadata( const char* name )
    {
        for( const Metadata* metadata : Metadata::GetAll() )
        {
            if( std::strcmp( metadata->name, name ) == 0 )
            {
                return *metadata;
            }
        }
        throw ConversionError{};
    }

    template<typename MEMBERS>
    size_t FindMember( const MEMBERS& members, const char* name, int dimension )
    {
        for( size_t i = 0; i < members.size(); i++ )
        {
            if( std::strcmp( members[i].name, name ) == 0 && members[i].dimensionIdx == dimension )
            {
                return i;
            }
        }
        throw ConversionError{};
    }

    // A node in the current format under construction. Members are looked up by
    // name, so a rename in the current metadata fails the conversion instead of
    // setting the wrong member.
    struct Node
    {
        NodeData* data;

        Node& Var( const char* name, float value, int dimension = -1 )
        {
            data->variables[FindMember( data->metadata->memberVariables, name, dimension )] = Metadata::MemberVariable::ValueUnion( value );
            return *this;
        }

        Node& VarInt( const char* name, int value, int dimension = -1 )
        {
            data->variables[FindMember( data->metadata->memberVariables, name, dimension )] = Metadata::MemberVariable::ValueUnion( value );
            return *this;
        }

        Node& Lookup( const char* name, NodeData* source, int dimension = -1 )
        {
            data->nodeLookups[FindMember( data->metadata->memberNodeLookups, name, dimension )] = source;
            return *this;
        }

        Node& HybridValue( const char* name, float value, int dimension = -1 )
        {
            auto& hybrid = data->hybrids[FindMember( data->metadata->memberHybrids, name, dimension )];
            hybrid.first = nullptr;
            hybrid.second = value;
            return *this;
        }

        Node& HybridNode( const char* name, NodeData* source, int dimension = -1 )
        {
            data->hybrids[FindMember( data->metadata->memberHybrids, name, dimension )].first = source;
            return *this;
        }
    };

    // The old Fractal::CalculateFractalBounding. A gain driven by a node counted
    // as 1 (its setter stored 1 as the constant).
    float LegacyFractalBounding( const LegacyNode::Hybrid& gain, int octaves )
    {
        float absGain = gain.node ? 1.0f : std::abs( gain.value );
        float amp = absGain;
        float ampFractal = 1.0f;
        for( int i = 1; i < octaves; i++ )
        {
            ampFractal += amp;
            amp *= absGain;
        }
        return 1.0f / ampFractal;
    }

    class Converter
    {
    public:
        explicit Converter( std::vector<std::string>* notes ) : mNotes( notes ) {}

        NodeData* Convert( const LegacyNode* legacy )
        {
            auto found = mConverted.find( legacy );
            if( found != mConverted.end() )
            {
                return found->second;
            }

            NodeData* converted = ConvertNode( *legacy );
            mConverted.emplace( legacy, converted );
            return converted;
        }

    private:
        Node Make( const char* name )
        {
            mStorage.emplace_back( new NodeData( &FindMetadata( name ) ) );
            return Node{ mStorage.back().get() };
        }

        // Coherent noise sampled at the coordinates it is given, as before 1.0.
        Node MakeCoherent( const char* name )
        {
            return Make( name ).Var( "Feature Scale", 1.0f );
        }

        void Hybrid( Node node, const char* name, const LegacyNode::Hybrid& hybrid, int dimension = -1 )
        {
            if( hybrid.node )
            {
                node.HybridNode( name, Convert( hybrid.node ), dimension );
            }
            else
            {
                node.HybridValue( name, hybrid.value, dimension );
            }
        }

        // `value * factor`, with the factor dropped when it is 1.
        NodeData* Scaled( NodeData* value, float factor )
        {
            if( factor == 1.0f )
            {
                return value;
            }
            return Make( "Multiply" ).Lookup( "LHS", value ).HybridValue( "RHS", factor ).data;
        }

        void Note( const char* text )
        {
            if( !mNotes )
            {
                return;
            }
            for( const std::string& existing : *mNotes )
            {
                if( existing == text )
                {
                    return;
                }
            }
            mNotes->emplace_back( text );
        }

        NodeData* Fractal( const char* name, const LegacyNode& legacy, NodeData* source )
        {
            Node fractal = Make( name )
                .Lookup( "Source", source )
                .VarInt( "Octaves", legacy.Int( 0 ) )
                .Var( "Lacunarity", legacy.Float( 1 ) );

            Hybrid( fractal, "Gain", legacy.hybrids[0] );
            Hybrid( fractal, "Weighted Strength", legacy.hybrids[1] );

            return Scaled( fractal.data, LegacyFractalBounding( legacy.hybrids[0], legacy.Int( 0 ) ) );
        }

        NodeData* DomainWarpFractal( const char* name, const LegacyNode& legacy )
        {
            const LegacyNode* warp = legacy.lookups[0];
            if( warp->id != kDomainWarpGradient )
            {
                throw ConversionError{};
            }

            // The fractal reads the amplitude of its source warp node, and before
            // 1.0 scaled it by the fractal bounding. That scale moves onto a copy
            // of the warp node, so any other use of the original is unaffected.
            float bounding = LegacyFractalBounding( legacy.hybrids[0], legacy.Int( 0 ) );
            Node scaledWarp = DomainWarpGradient( *warp );

            if( warp->hybrids[0].node )
            {
                scaledWarp.HybridNode( "Warp Amplitude", Scaled( Convert( warp->hybrids[0].node ), bounding ) );
            }
            else
            {
                scaledWarp.HybridValue( "Warp Amplitude", warp->hybrids[0].value * bounding );
            }

            Node fractal = Make( name )
                .Lookup( "Domain Warp Source", scaledWarp.data )
                .VarInt( "Octaves", legacy.Int( 0 ) )
                .Var( "Lacunarity", legacy.Float( 1 ) );

            Hybrid( fractal, "Gain", legacy.hybrids[0] );
            Hybrid( fractal, "Weighted Strength", legacy.hybrids[1] );

            Note( "Domain warp: hashes changed in 1.0, warped positions differ" );
            return fractal.data;
        }

        // A CellularDistance node returning `returnType` of the given indices, with
        // the output range set so it returns raw distances as before 1.0.
        NodeData* CellularDistanceRaw( const LegacyNode& legacy, int index0, int index1, int returnType )
        {
            Node cellular = MakeCoherent( "CellularDistance" )
                .VarInt( "Distance Function", legacy.Int( 0 ) )
                .VarInt( "Distance Index 0", index0 )
                .VarInt( "Distance Index 1", index1 )
                .VarInt( "Return Type", returnType );
            Hybrid( cellular, "Grid Jitter", legacy.hybrids[0] );

            // Before 1.0 this node returned raw distances; 1.0 maps its native
            // range onto [Output Min, Output Max]. Setting that range to the
            // native range gives the raw distance back. The native range depends
            // on the sampled dimension, so this is exact in 2D, and within about
            // 6% (3D) or 11% (4D) elsewhere.
            float maxDistance = ( 1.0f + kCellularJitter2D ) * ( 1.0f + kCellularJitter2D );
            float nativeMax = maxDistance;
            switch( returnType )
            {
            case kIndex0Add1: nativeMax = maxDistance * 2.0f; break;
            case kIndex0Mul1: nativeMax = maxDistance * maxDistance; break;
            case kIndex0Sub1:
                // Distances are sorted nearest first, so before 1.0 this was
                // negative when Index 0 < Index 1; 1.0 returns the magnitude.
                if( std::min( std::max( index0, 0 ), 3 ) < std::min( std::max( index1, 0 ), 3 ) )
                {
                    nativeMax = -maxDistance;
                }
                break;
            default: break;
            }

            return cellular.Var( "Output Min", 0.0f ).Var( "Output Max", nativeMax ).data;
        }

        NodeData* SquareRoot( NodeData* source )
        {
            return Make( "SignedSquareRoot" ).Lookup( "Source", source ).data;
        }

        NodeData* CellularDistance( const LegacyNode& legacy )
        {
            int index0 = legacy.Int( 1 );
            int index1 = legacy.Int( 2 );
            int returnType = legacy.Int( 3 );

            if( legacy.Int( 0 ) != kEuclidean )
            {
                return CellularDistanceRaw( legacy, index0, index1, returnType );
            }

            // Before 1.0 a Euclidean CellularDistance took the square root twice
            // (once per cell, again on return), so it returned the square root of
            // each distance. 1.0 returns the distances. A square root on the output
            // restores Index0, Mul and Div; Add and Sub combine two roots, so they
            // become two Index0 nodes. Both share the seed and jitter, so they see
            // the same cells.
            //
            // Both versions root the selected distances in place, one slot per
            // index, so equal indices root the same slot twice. Each Index0 node
            // keeps the other index too: then every slot is rooted exactly as
            // often as before 1.0, including that case.
            switch( returnType )
            {
            case kIndex0Add1:
            case kIndex0Sub1:
            {
                NodeData* first = SquareRoot( CellularDistanceRaw( legacy, index0, index1, kIndex0 ) );
                NodeData* second = SquareRoot( CellularDistanceRaw( legacy, index1, index0, kIndex0 ) );
                if( returnType == kIndex0Add1 )
                {
                    return Make( "Add" ).Lookup( "LHS", first ).HybridNode( "RHS", second ).data;
                }
                return Make( "Subtract" ).HybridNode( "LHS", first ).HybridNode( "RHS", second ).data;
            }
            default:
                return SquareRoot( CellularDistanceRaw( legacy, index0, index1, returnType ) );
            }
        }

        Node DomainWarpGradient( const LegacyNode& legacy )
        {
            // Warp Frequency multiplied the warp lattice coordinates; Feature Scale
            // divides them.
            Node warp = Make( "DomainWarpGradient" )
                .Var( "Feature Scale", 1.0f / legacy.Float( 0 ) )
                .Lookup( "Source", Convert( legacy.lookups[0] ) );

            Hybrid( warp, "Warp Amplitude", legacy.hybrids[0] );
            return warp;
        }

        NodeData* ConvertNode( const LegacyNode& legacy )
        {
            switch( legacy.id )
            {
            case kConstant:
                return Make( "Constant" ).Var( "Value", legacy.Float( 0 ) ).data;

            case kWhite:
                Note( "White: hashes changed in 1.0" );
                return Make( "White" ).data;

            case kCheckerboard:
                // Before 1.0 even cells were +1 and odd cells -1; 1.0 outputs the
                // opposite, so the range is inverted.
                return Make( "Checkerboard" )
                    .Var( "Feature Scale", legacy.Float( 0 ) )
                    .Var( "Output Min", 1.0f )
                    .Var( "Output Max", -1.0f ).data;

            case kSineWave:
                return Make( "SineWave" ).Var( "Feature Scale", legacy.Float( 0 ) ).data;

            case kPositionOutput:
            {
                Node gradient = Make( "Gradient" );
                for( int d = 0; d < 4; d++ )
                {
                    gradient.Var( "Multiplier", legacy.Float( d ), d );
                    gradient.HybridValue( "Offset", legacy.Float( 4 + d ), d );
                }
                return gradient.data;
            }

            case kDistanceToPoint:
            {
                Node distance = Make( "DistanceToPoint" ).VarInt( "Distance Function", legacy.Int( 0 ) );
                for( int d = 0; d < 4; d++ )
                {
                    distance.HybridValue( "Point", legacy.Float( 1 + d ), d );
                }
                return distance.data;
            }

            case kValue:
                Note( "Value: hashes changed in 1.0" );
                return MakeCoherent( "Value" ).data;

            case kPerlin:
                Note( "Perlin: hashes changed in 1.0" );
                return MakeCoherent( "Perlin" ).data;

            case kSimplex:
                Note( "Simplex: hashes and the simplex implementation changed in 1.0" );
                return MakeCoherent( "Simplex" ).data;

            case kOpenSimplex2:
                Note( "OpenSimplex2: replaced by Simplex in 1.0" );
                return MakeCoherent( "Simplex" ).data;

            case kOpenSimplex2S:
                Note( "OpenSimplex2S: renamed SuperSimplex in 1.0, hashes changed" );
                return MakeCoherent( "SuperSimplex" ).data;

            case kCellularValue:
            {
                Node cellular = MakeCoherent( "CellularValue" )
                    .VarInt( "Distance Function", legacy.Int( 0 ) )
                    .VarInt( "Value Index", legacy.Int( 1 ) );
                Hybrid( cellular, "Grid Jitter", legacy.hybrids[0] );
                Note( "Cellular: hashes changed in 1.0, cell positions differ" );
                return cellular.data;
            }

            case kCellularDistance:
                Note( "CellularDistance: output range reproduced for 2D sampling; 3D and 4D differ by a constant factor" );
                Note( "Cellular: hashes changed in 1.0, cell positions differ" );
                return CellularDistance( legacy );

            case kCellularLookup:
            {
                // Before 1.0 the lookup was sampled at the cell centre times the
                // lookup frequency; 1.0 samples it at the cell centre.
                NodeData* lookup = Convert( legacy.lookups[0] );
                float lookupFrequency = legacy.Float( 1 );
                if( lookupFrequency != 1.0f )
                {
                    lookup = Make( "DomainScale" ).Lookup( "Source", lookup ).Var( "Scaling", lookupFrequency ).data;
                }

                Node cellular = MakeCoherent( "CellularLookup" )
                    .VarInt( "Distance Function", legacy.Int( 0 ) )
                    .Lookup( "Lookup", lookup );
                Hybrid( cellular, "Grid Jitter", legacy.hybrids[0] );
                Note( "Cellular: hashes changed in 1.0, cell positions differ" );
                return cellular.data;
            }

            case kFractalFBm:
                return Fractal( "FractalFBm", legacy, Convert( legacy.lookups[0] ) );

            case kFractalRidged:
                return Fractal( "FractalRidged", legacy, Convert( legacy.lookups[0] ) );

            case kFractalPingPong:
            {
                // FractalPingPong applied, to every octave:
                //   t = ( source + 1 ) * strength
                //   t - 2 * round( t / 2 )      (then t < 1 ? t : 2 - t, a no-op
                //                                since that value never exceeds 1)
                // which is a sawtooth in [-1, 1], not a ping-pong: the rounding
                // folded half of every period negative. 1.0's PingPong node fixed
                // that, so the legacy sawtooth is rebuilt instead, to keep the
                // output the author tuned: Terrace at step count 0.5 is exactly
                // 2 * round( t / 2 ).
                NodeData* shifted = Make( "Add" )
                    .Lookup( "LHS", Convert( legacy.lookups[0] ) )
                    .HybridValue( "RHS", 1.0f ).data;

                Node scaled = Make( "Multiply" ).Lookup( "LHS", shifted );
                Hybrid( scaled, "RHS", legacy.hybrids[2] );
                if( legacy.hybrids[2].node )
                {
                    Note( "FractalPingPong: a node-driven Ping Pong Strength is now sampled at each octave's position, not once at the fractal's" );
                }

                NodeData* rounded = Make( "Terrace" )
                    .Lookup( "Source", scaled.data )
                    .Var( "Step Count", 0.5f ).data;

                NodeData* sawtooth = Make( "Subtract" )
                    .HybridNode( "LHS", scaled.data )
                    .HybridNode( "RHS", rounded ).data;

                return Fractal( "FractalFBm", legacy, sawtooth );
            }

            case kDomainWarpGradient:
                Note( "Domain warp: hashes changed in 1.0, warped positions differ" );
                return DomainWarpGradient( legacy ).data;

            case kDomainWarpFractalProgressive:
                return DomainWarpFractal( "DomainWarpFractalProgressive", legacy );

            case kDomainWarpFractalIndependant:
                return DomainWarpFractal( "DomainWarpFractalIndependent", legacy );

            case kDomainScale:
                return Make( "DomainScale" )
                    .Lookup( "Source", Convert( legacy.lookups[0] ) )
                    .Var( "Scaling", legacy.Float( 0 ) ).data;

            case kDomainOffset:
            {
                Node offset = Make( "DomainOffset" ).Lookup( "Source", Convert( legacy.lookups[0] ) );
                for( int d = 0; d < 4; d++ )
                {
                    Hybrid( offset, "Offset", legacy.hybrids[d], d );
                }
                return offset.data;
            }

            case kDomainRotate:
                return Make( "DomainRotate" )
                    .Lookup( "Source", Convert( legacy.lookups[0] ) )
                    .Var( "Yaw", legacy.Float( 0 ) )
                    .Var( "Pitch", legacy.Float( 1 ) )
                    .Var( "Roll", legacy.Float( 2 ) ).data;

            case kSeedOffset:
                return Make( "SeedOffset" )
                    .Lookup( "Source", Convert( legacy.lookups[0] ) )
                    .VarInt( "Seed Offset", legacy.Int( 0 ) ).data;

            case kRemap:
                return Make( "Remap" )
                    .Lookup( "Source", Convert( legacy.lookups[0] ) )
                    .HybridValue( "From Min", legacy.Float( 0 ) )
                    .HybridValue( "From Max", legacy.Float( 1 ) )
                    .HybridValue( "To Min", legacy.Float( 2 ) )
                    .HybridValue( "To Max", legacy.Float( 3 ) ).data;

            case kConvertRGBA8:
                return Make( "ConvertRGBA8" )
                    .Lookup( "Source", Convert( legacy.lookups[0] ) )
                    .Var( "Min", legacy.Float( 0 ) )
                    .Var( "Max", legacy.Float( 1 ) ).data;

            case kAdd:
            case kMultiply:
            case kMin:
            case kMax:
            {
                static const char* const kNames[] = { "Add", "Multiply", "Min", "Max" };
                const char* name = kNames[legacy.id == kAdd ? 0 : legacy.id == kMultiply ? 1 : legacy.id == kMin ? 2 : 3];
                Node op = Make( name ).Lookup( "LHS", Convert( legacy.lookups[0] ) );
                Hybrid( op, "RHS", legacy.hybrids[0] );
                return op.data;
            }

            case kSubtract:
            case kDivide:
            {
                Node op = Make( legacy.id == kSubtract ? "Subtract" : "Divide" );
                Hybrid( op, "LHS", legacy.hybrids[0] );
                Hybrid( op, "RHS", legacy.hybrids[1] );
                return op.data;
            }

            case kMinSmooth:
            case kMaxSmooth:
            {
                Node op = Make( legacy.id == kMinSmooth ? "MinSmooth" : "MaxSmooth" ).Lookup( "LHS", Convert( legacy.lookups[0] ) );
                Hybrid( op, "RHS", legacy.hybrids[0] );
                Hybrid( op, "Smoothness", legacy.hybrids[1] );
                return op.data;
            }

            case kFade:
            {
                // Before 1.0: lerp( A, B, |fade| ), unclamped. 1.0 maps the fade
                // from [Fade Min, Fade Max] and clamps it to [0, 1].
                Node fade = Make( "Fade" )
                    .Lookup( "A", Convert( legacy.lookups[0] ) )
                    .Lookup( "B", Convert( legacy.lookups[1] ) )
                    .HybridValue( "Fade Min", 0.0f )
                    .HybridValue( "Fade Max", 1.0f );

                const LegacyNode::Hybrid& amount = legacy.hybrids[0];
                if( amount.node )
                {
                    fade.HybridNode( "Fade", Make( "Abs" ).Lookup( "Source", Convert( amount.node ) ).data );
                }
                else
                {
                    fade.HybridValue( "Fade", std::abs( amount.value ) );
                }
                Note( "Fade: 1.0 clamps the fade amount to [0, 1]; the old node extrapolated beyond it" );
                return fade.data;
            }

            case kTerrace:
                return Make( "Terrace" )
                    .Lookup( "Source", Convert( legacy.lookups[0] ) )
                    .Var( "Step Count", legacy.Float( 0 ) )
                    .HybridValue( "Smoothness", legacy.Float( 1 ) ).data;

            case kPowFloat:
            {
                Node pow = Make( "PowFloat" );
                Hybrid( pow, "Value", legacy.hybrids[0] );
                Hybrid( pow, "Pow", legacy.hybrids[1] );
                Note( "PowFloat: 1.0 raises |value|, so a negative value no longer produces NaN" );
                return pow.data;
            }

            case kPowInt:
                return Make( "PowInt" )
                    .Lookup( "Value", Convert( legacy.lookups[0] ) )
                    .VarInt( "Pow", legacy.Int( 0 ) ).data;

            case kDomainAxisScale:
            {
                Node scale = Make( "DomainAxisScale" ).Lookup( "Source", Convert( legacy.lookups[0] ) );
                for( int d = 0; d < 4; d++ )
                {
                    scale.Var( "Scaling", legacy.Float( d ), d );
                }
                return scale.data;
            }

            case kAddDimension:
            {
                Node add = Make( "AddDimension" ).Lookup( "Source", Convert( legacy.lookups[0] ) );
                Hybrid( add, "New Dimension Position", legacy.hybrids[0] );
                return add.data;
            }

            case kRemoveDimension:
                return Make( "RemoveDimension" )
                    .Lookup( "Source", Convert( legacy.lookups[0] ) )
                    .VarInt( "Remove Dimension", legacy.Int( 0 ) ).data;

            case kGeneratorCache:
                return Make( "GeneratorCache" ).Lookup( "Source", Convert( legacy.lookups[0] ) ).data;

            default:
                throw ConversionError{};
            }
        }

        std::vector<std::string>* mNotes;
        std::vector<std::unique_ptr<NodeData>> mStorage;
        std::unordered_map<const LegacyNode*, NodeData*> mConverted;
    };
}

std::string FastNoise::Legacy::ConvertEncodedNodeTree( const char* legacyEncodedNodeTree, std::vector<std::string>* notes )
{
    if( !legacyEncodedNodeTree || !*legacyEncodedNodeTree )
    {
        return {};
    }

    try
    {
        LegacyReader reader( Base64::Decode( legacyEncodedNodeTree ) );
        const LegacyNode* root = reader.ReadTree();

        std::vector<std::string> localNotes;
        Converter converter( notes ? &localNotes : nullptr );
        NodeData* converted = converter.Convert( root );

        std::string encoded = Metadata::SerialiseNodeData( converted, false );
        if( !encoded.empty() && notes )
        {
            notes->insert( notes->end(), localNotes.begin(), localNotes.end() );
        }
        return encoded;
    }
    catch( const ConversionError& )
    {
        return {};
    }
}
