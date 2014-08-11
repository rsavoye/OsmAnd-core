#include "AtlasMapRendererSymbolsStage.h"

#include "QtExtensions.h"
#include "ignore_warnings_on_external_includes.h"
#include <QLinkedList>
#include "restore_internal_warnings.h"

#include "ignore_warnings_on_external_includes.h"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
#include "restore_internal_warnings.h"

#include "OsmAndCore.h"
#include "AtlasMapRenderer.h"
#include "AtlasMapRendererDebugStage.h"
#include "MapSymbol.h"
#include "VectorMapSymbol.h"
#include "BillboardVectorMapSymbol.h"
#include "OnSurfaceVectorMapSymbol.h"
#include "OnPathMapSymbol.h"
#include "BillboardRasterMapSymbol.h"
#include "OnSurfaceRasterMapSymbol.h"
#include "MapSymbolsGroup.h"
#include "QKeyValueIterator.h"
#include "ObjectWithId.h"

OsmAnd::AtlasMapRendererSymbolsStage::AtlasMapRendererSymbolsStage(AtlasMapRenderer* const renderer_)
    : AtlasMapRendererStage(renderer_)
{
}

OsmAnd::AtlasMapRendererSymbolsStage::~AtlasMapRendererSymbolsStage()
{
}

void OsmAnd::AtlasMapRendererSymbolsStage::obtainRenderableSymbols(
    QList< std::shared_ptr<const RenderableSymbol> >& outRenderableSymbols) const
{
    typedef QLinkedList< std::shared_ptr<const RenderableSymbol> > PlottedSymbols;
    struct PlottedSymbolRef
    {
        PlottedSymbols::iterator iterator;
        std::shared_ptr<const MapSymbol> mapSymbol;
    };

    QReadLocker scopedLocker(&publishedMapSymbolsLock);

    const auto& internalState = getInternalState();

    // Iterate over map symbols layer sorted by "order" in ascending direction
    IntersectionsQuadTree intersections(currentState.viewport, 8);
    PlottedSymbols plottedSymbols;
    QHash< const MapSymbolsGroup*, QList< PlottedSymbolRef > > plottedMapSymbolsByGroup;
    for (const auto& publishedMapSymbols : constOf(publishedMapSymbols))
    {
        // Obtain renderables in order how they should be rendered
        QMultiMap< float, std::shared_ptr<RenderableSymbol> > sortedRenderables;
        if (!debugSettings->excludeOnPathSymbolsFromProcessing)
            processOnPathSymbols(publishedMapSymbols, sortedRenderables);
        if (!debugSettings->excludeBillboardSymbolsFromProcessing)
            processBillboardSymbols(publishedMapSymbols, sortedRenderables);
        if (!debugSettings->excludeOnSurfaceSymbolsFromProcessing)
            processOnSurfaceSymbols(publishedMapSymbols, sortedRenderables);

        // Plot symbols in reversed order, since sortedSymbols contains symbols by distance from camera from near->far.
        // And rendering needs to be done far->near, as well as plotting
        auto itRenderableEntry = iteratorOf(sortedRenderables);
        itRenderableEntry.toBack();
        while (itRenderableEntry.hasPrevious())
        {
            const auto& entry = itRenderableEntry.previous();
            const auto renderable = entry.value();

            bool plotted = false;
            if (const auto& renderable_ = std::dynamic_pointer_cast<RenderableBillboardSymbol>(renderable))
            {
                plotted = plotBillboardSymbol(
                    renderable_,
                    intersections);
            }
            else if (const auto& renderable_ = std::dynamic_pointer_cast<RenderableOnPathSymbol>(renderable))
            {
                plotted = plotOnPathSymbol(
                    renderable_,
                    intersections);
            }
            else if (const auto& renderable_ = std::dynamic_pointer_cast<RenderableOnSurfaceSymbol>(renderable))
            {
                plotted = plotOnSurfaceSymbol(
                    renderable_,
                    intersections);
            }

            if (plotted)
            {
                const auto itPlottedSymbol = plottedSymbols.insert(plottedSymbols.end(), renderable);
                PlottedSymbolRef plottedSymbolRef = { itPlottedSymbol, renderable->mapSymbol };

                plottedMapSymbolsByGroup[renderable->mapSymbol->groupPtr].push_back(qMove(plottedSymbolRef));
            }
        }
    }

    // Remove those plotted symbols that do not conform to presentation rules
    auto itPlottedSymbolsGroup = mutableIteratorOf(plottedMapSymbolsByGroup);
    while (itPlottedSymbolsGroup.hasNext())
    {
        auto& plottedGroupSymbols = itPlottedSymbolsGroup.next().value();

        const auto mapSymbolGroup = plottedGroupSymbols.first().mapSymbol->group.lock();
        if (!mapSymbolGroup)
        {
            // Discard entire group
            for (const auto& plottedGroupSymbol : constOf(plottedGroupSymbols))
                plottedSymbols.erase(plottedGroupSymbol.iterator);

            itPlottedSymbolsGroup.remove();
            continue;
        }

        // Just skip all rules
        if (mapSymbolGroup->presentationMode & MapSymbolsGroup::PresentationModeFlag::ShowAnything)
            continue;

        // Rule: show all symbols or no symbols
        if (mapSymbolGroup->presentationMode & MapSymbolsGroup::PresentationModeFlag::ShowAllOrNothing)
        {
            if (mapSymbolGroup->symbols.size() != plottedGroupSymbols.size())
            {
                // Discard entire group
                for (const auto& plottedGroupSymbol : constOf(plottedGroupSymbols))
                    plottedSymbols.erase(plottedGroupSymbol.iterator);

                itPlottedSymbolsGroup.remove();
                continue;
            }
        }

        // Rule: if there's icon, icon must always be visible. Otherwise discard entire group
        if (mapSymbolGroup->presentationMode & MapSymbolsGroup::PresentationModeFlag::ShowNoneIfIconIsNotShown)
        {
            const auto symbolWithIconContentClass = mapSymbolGroup->getFirstSymbolWithContentClass(MapSymbol::ContentClass::Icon);
            if (symbolWithIconContentClass)
            {
                bool iconPlotted = false;
                for (const auto& plottedGroupSymbol : constOf(plottedGroupSymbols))
                {
                    if (plottedGroupSymbol.mapSymbol == symbolWithIconContentClass)
                    {
                        iconPlotted = true;
                        break;
                    }
                }

                if (!iconPlotted)
                {
                    // Discard entire group
                    for (const auto& plottedGroupSymbol : constOf(plottedGroupSymbols))
                        plottedSymbols.erase(plottedGroupSymbol.iterator);

                    itPlottedSymbolsGroup.remove();
                    continue;
                }
            }
        }

        // Rule: if at least one caption was not shown, discard all other captions
        if (mapSymbolGroup->presentationMode & MapSymbolsGroup::PresentationModeFlag::ShowAllCaptionsOrNoCaptions)
        {
            const auto captionsCount = mapSymbolGroup->numberOfSymbolsWithContentClass(MapSymbol::ContentClass::Caption);
            if (captionsCount > 0)
            {
                unsigned int captionsPlotted = 0;
                for (const auto& plottedGroupSymbol : constOf(plottedGroupSymbols))
                {
                    if (plottedGroupSymbol.mapSymbol->contentClass == MapSymbol::ContentClass::Caption)
                        captionsPlotted++;
                }

                if (captionsCount != captionsPlotted)
                {
                    // Discard all plotted captions from group
                    auto itPlottedGroupSymbol = mutableIteratorOf(plottedGroupSymbols);
                    while (itPlottedGroupSymbol.hasNext())
                    {
                        const auto& plottedGroupSymbol = itPlottedGroupSymbol.next();

                        if (plottedGroupSymbol.mapSymbol->contentClass != MapSymbol::ContentClass::Caption)
                            continue;

                        plottedSymbols.erase(plottedGroupSymbol.iterator);
                        itPlottedGroupSymbol.remove();
                    }
                }
            }
        }
    }

    // Publish the result
    outRenderableSymbols.clear();
    outRenderableSymbols.reserve(plottedSymbols.size());
    for (const auto& plottedSymbol : constOf(plottedSymbols))
        outRenderableSymbols.push_back(plottedSymbol);
}

void OsmAnd::AtlasMapRendererSymbolsStage::processOnPathSymbols(
    const MapRenderer::PublishedMapSymbols& input,
    QMultiMap< float, std::shared_ptr<RenderableSymbol> >& output) const
{
    // Process on path symbols to get set of renderables
    QList< std::shared_ptr<RenderableOnPathSymbol> > renderables;
    obtainRenderablesFromOnPathSymbols(input, renderables);

    // Sort visible SOPs by distance to camera
    sortRenderablesFromOnPathSymbols(renderables, output);
}

void OsmAnd::AtlasMapRendererSymbolsStage::obtainRenderablesFromOnPathSymbols(
    const MapRenderer::PublishedMapSymbols& input,
    QList< std::shared_ptr<RenderableOnPathSymbol> >& output) const
{
    const auto& internalState = getInternalState();

    for (const auto& symbolEntry : rangeOf(constOf(input)))
    {
        const auto& currentSymbol_ = symbolEntry.key();
        if (currentSymbol_->isHidden)
            continue;
        const auto currentSymbol = std::dynamic_pointer_cast<const OnPathMapSymbol>(currentSymbol_);
        if (!currentSymbol)
            continue;
        const auto& points31 = currentSymbol->path;

        // Path must have at least 2 points
        if (Q_UNLIKELY(points31.size() < 2))
        {
            assert(false);
            continue;
        }

        // Capture group of this symbol to get widths of all symbols
        const auto& mapSymbolsGroup = currentSymbol->group.lock();
        if (!mapSymbolsGroup)
        {
            // Group has to be present, there's no way to process this without group
            if (Q_UNLIKELY(debugSettings->showAllPaths))
            {
                QVector< glm::vec3 > debugPoints;
                for (const auto& pointInWorld : convertPoints31ToWorld(currentSymbol->path))
                {
                    debugPoints.push_back(qMove(glm::vec3(
                        pointInWorld.x,
                        0.0f,
                        pointInWorld.y)));
                }
                getRenderer()->debugStage->addLine3D(debugPoints, SK_ColorWHITE);
            }
            continue;
        }

        // Ordering of OnPathSymbols is maintained, regardless of locale or whatever.
        // They will appear on path in the order they are stored in group.

        // Calculate widths of entire on-path-symbols in group and width of symbols before current symbol
        float totalWidth = 0.0f;
        float widthBeforeCurrentSymbol = 0.0f;
        for (const auto& otherSymbol_ : constOf(mapSymbolsGroup->symbols))
        {
            // Verify that other symbol is also OnPathSymbol
            const auto otherSymbol = std::dynamic_pointer_cast<const OnPathMapSymbol>(otherSymbol_);
            if (!otherSymbol)
                continue;

            if (otherSymbol == currentSymbol)
                widthBeforeCurrentSymbol = totalWidth;
            totalWidth += otherSymbol->size.x;
        }

        //////////////////////////////////////////////////////////////////////////
        /*if (mapSymbolsGroup->symbols.first() != currentSymbol_)
            continue;*/
        //////////////////////////////////////////////////////////////////////////

        // Calculate current path in world and screen coordinates.
        // NOTE: There's an assumption that all OnPathSymbols from same group share same path
        const auto pathInWorld = convertPoints31ToWorld(currentSymbol->path);
        const auto pathOnScreen = projectFromWorldToScreen(pathInWorld);

        // First "plot" virtual renderable that occupies length of "widthBeforeCurrentSymbol" pixels
        unsigned int originPointIndex = 0;
        float originOccupiedLength = 0.0f;
        bool originOccupiedLengthIsIn2D = true;
        if (widthBeforeCurrentSymbol > 0.0f)
        {
            const auto offsetBeforeCurrentSymbolFits = tryToFindSpaceOnPath(
                currentSymbol->path.size(),
                pathInWorld,
                pathOnScreen,
                widthBeforeCurrentSymbol,
                0,
                true,
                0.0f,
                originPointIndex,
                originOccupiedLengthIsIn2D,
                originOccupiedLength);

            // In case even offset failed to fit, nothing can be done
            if (!offsetBeforeCurrentSymbolFits)
                continue;
        }

        // Get GPU resource for this map symbol
        const auto gpuResource = std::dynamic_pointer_cast<const GPUAPI::TextureInGPU>(captureGpuResource(symbolEntry.value(), currentSymbol_));
        if (!gpuResource)
        {
            if (Q_UNLIKELY(debugSettings->showAllPaths))
            {
                QVector< glm::vec3 > debugPoints;
                for (const auto& pointInWorld : pathInWorld)
                {
                    debugPoints.push_back(qMove(glm::vec3(
                        pointInWorld.x,
                        0.0f,
                        pointInWorld.y)));
                }
                getRenderer()->debugStage->addLine3D(debugPoints, SK_ColorCYAN);
            }

            continue;
        }

        // Draw this path fully only once
        if (Q_UNLIKELY(debugSettings->showAllPaths) &&
            currentSymbol_ == mapSymbolsGroup->symbols.first())
        {
            QVector< glm::vec3 > debugPoints;
            for (const auto& pointInWorld : pathInWorld)
            {
                debugPoints.push_back(qMove(glm::vec3(
                    pointInWorld.x,
                    0.0f,
                    pointInWorld.y)));
            }
            getRenderer()->debugStage->addLine3D(debugPoints, SK_ColorGRAY);
        }
        
        // Try to fit as many instances of current symbol as possible
        unsigned int symbolInstancesFitted = 0;
        unsigned int nextOriginPointIndex = originPointIndex;
        float nextOriginOccupiedLength = originOccupiedLength;
        bool nextOriginOccupiedLengthIsIn2D = originOccupiedLengthIsIn2D;
        for (;;)
        {
            const auto subpathStartIndex = nextOriginPointIndex;
            const auto currentSymbolInstanceFits = tryToFindSpaceOnPath(
                currentSymbol->path.size(),
                pathInWorld,
                pathOnScreen,
                currentSymbol->size.x,
                nextOriginPointIndex,
                nextOriginOccupiedLengthIsIn2D,
                nextOriginOccupiedLength,
                nextOriginPointIndex,
                nextOriginOccupiedLengthIsIn2D,
                nextOriginOccupiedLength);
            
            // Stop in case current symbol doesn't fit anymore
            if (!currentSymbolInstanceFits)
            {
                // If current symbol is the first one and it doesn't fit, show it
                if (Q_UNLIKELY(debugSettings->showTooShortOnPathSymbolsRenderablesPaths) &&
                    currentSymbol_ == mapSymbolsGroup->symbols.first() && 
                    symbolInstancesFitted == 0)
                {
                    QVector< glm::vec3 > debugPoints;
                    for (const auto& pointInWorld : pathInWorld)
                    {
                        debugPoints.push_back(qMove(glm::vec3(
                            pointInWorld.x,
                            0.0f,
                            pointInWorld.y)));
                    }
                    getRenderer()->debugStage->addLine3D(debugPoints, SkColorSetA(SK_ColorYELLOW, 128));
                }

                break;
            }

            // Actually plot the instance
            const auto is2D = nextOriginOccupiedLengthIsIn2D;
            const auto subpathEndIndex = nextOriginPointIndex;
            const auto subpathSize = subpathEndIndex - subpathStartIndex + 1;
            const auto offsetFromStart = nextOriginOccupiedLength;
            std::shared_ptr<RenderableOnPathSymbol> renderable(new RenderableOnPathSymbol());
            renderable->mapSymbol = currentSymbol_;
            renderable->gpuResource = gpuResource;
            renderable->is2D = is2D;
            renderable->distanceToCamera = computeDistanceBetweenCameraToPath(
                pathInWorld,
                subpathStartIndex,
                subpathEndIndex);
            renderable->directionInWorld = computeSubpathDirection(
                pathInWorld,
                subpathStartIndex,
                subpathEndIndex);
            renderable->directionOnScreen = computeSubpathDirection(
                pathOnScreen,
                subpathStartIndex,
                subpathEndIndex);
            /*renderable->glyphsPlacement = computePlacementOfGlyphsOnPath(
                renderable->is2D,
                pathInWorld,
                pathOnScreen,
                subpathStartIndex,
                subpathEndIndex,
                nextOriginOccupiedLength);
            output.push_back(qMove(renderable));*/
            symbolInstancesFitted++;

            if (Q_UNLIKELY(debugSettings->showOnPathSymbolsRenderablesPaths))
            {
                const glm::vec2 directionOnScreenN(-renderable->directionOnScreen.y, renderable->directionOnScreen.x);

                // Path itself
                QVector< glm::vec3 > debugPoints;
                auto pPointInWorld = pathInWorld.constData() + subpathStartIndex;
                for (auto idx = 0u; idx < subpathSize; idx++, pPointInWorld++)
                {
                    debugPoints.push_back(qMove(glm::vec3(
                        pPointInWorld->x,
                        0.0f,
                        pPointInWorld->y)));
                }
                getRenderer()->debugStage->addLine3D(debugPoints, SkColorSetA(renderable->is2D ? SK_ColorGREEN : SK_ColorRED, 128));

                // Subpath N (start)
                {
                    QVector<glm::vec2> lineN;
                    const auto sn0 = pathOnScreen[subpathStartIndex];
                    lineN.push_back(glm::vec2(sn0.x, currentState.windowSize.y - sn0.y));
                    const auto sn1 = pathOnScreen[subpathStartIndex] + (directionOnScreenN*32.0f);
                    lineN.push_back(glm::vec2(sn1.x, currentState.windowSize.y - sn1.y));
                    getRenderer()->debugStage->addLine2D(lineN, SkColorSetA(SK_ColorCYAN, 128));
                }

                // Subpath N (end)
                {
                    QVector<glm::vec2> lineN;
                    const auto sn0 = pathOnScreen[subpathEndIndex];
                    lineN.push_back(glm::vec2(sn0.x, currentState.windowSize.y - sn0.y));
                    const auto sn1 = pathOnScreen[subpathEndIndex] + (directionOnScreenN*32.0f);
                    lineN.push_back(glm::vec2(sn1.x, currentState.windowSize.y - sn1.y));
                    getRenderer()->debugStage->addLine2D(lineN, SkColorSetA(SK_ColorMAGENTA, 128));
                }
            }

            // Find offset after current instance of the symbol before next instance of the symbol
            const auto offsetBeforeNextSymbolInstanceFits = tryToFindSpaceOnPath(
                currentSymbol->path.size(),
                pathInWorld,
                pathOnScreen,
                totalWidth - currentSymbol->size.x,
                nextOriginPointIndex,
                nextOriginOccupiedLengthIsIn2D,
                nextOriginOccupiedLength,
                nextOriginPointIndex,
                nextOriginOccupiedLengthIsIn2D,
                nextOriginOccupiedLength);
            if (!offsetBeforeNextSymbolInstanceFits)
                break;
        }
    }
}

QVector<glm::vec2> OsmAnd::AtlasMapRendererSymbolsStage::convertPoints31ToWorld(const QVector<PointI>& points31) const
{
    return convertPoints31ToWorld(points31, 0, points31.size() - 1);
}

QVector<glm::vec2> OsmAnd::AtlasMapRendererSymbolsStage::convertPoints31ToWorld(const QVector<PointI>& points31, unsigned int startIndex, unsigned int endIndex) const
{
    const auto& internalState = getInternalState();

    const auto count = endIndex - startIndex + 1;
    QVector<glm::vec2> result(count);
    auto pPointInWorld = result.data();
    auto pPoint31 = points31.constData() + startIndex;

    for (auto idx = 0u; idx < count; idx++)
    {
        *(pPointInWorld++) = Utilities::convert31toFloat(
                *(pPoint31++) - currentState.target31,
                currentState.zoomBase) * static_cast<float>(AtlasMapRenderer::TileSize3D);
    }

    return result;
}

QVector<glm::vec2> OsmAnd::AtlasMapRendererSymbolsStage::projectFromWorldToScreen(const QVector<glm::vec2>& pointsInWorld) const
{
    return projectFromWorldToScreen(pointsInWorld, 0, pointsInWorld.size() - 1);
}

QVector<glm::vec2> OsmAnd::AtlasMapRendererSymbolsStage::projectFromWorldToScreen(const QVector<glm::vec2>& pointsInWorld, unsigned int startIndex, unsigned int endIndex) const
{
    const auto& internalState = getInternalState();

    const auto count = endIndex - startIndex + 1;
    QVector<glm::vec2> result(count);
    auto pPointOnScreen = result.data();
    auto pPointInWorld = pointsInWorld.constData() + startIndex;

    for (auto idx = 0u; idx < count; idx++)
    {
        *(pPointOnScreen++) = glm::project(
            glm::vec3(pPointInWorld->x, 0.0f, pPointInWorld->y),
            internalState.mCameraView,
            internalState.mPerspectiveProjection,
            internalState.glmViewport).xy;
        pPointInWorld++;
    }

    return result;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::isInclineAllowedFor2D(const glm::vec2& pointOnScreen0, const glm::vec2& pointOnScreen1) const
{
    // Calculate 'incline' of line and compare to horizontal direction.
    // If any 'incline' is larger than 15 degrees, this line can not be rendered as 2D

    const static float inclineThresholdSinSq = 0.0669872981; // qSin(qDegreesToRadians(15.0f))*qSin(qDegreesToRadians(15.0f))

    const auto vSegment = pointOnScreen1 - pointOnScreen0;
    const auto d = vSegment.y;// horizont.x*vSegment.y - horizont.y*vSegment.x == 1.0f*vSegment.y - 0.0f*vSegment.x
    const auto inclineSinSq = d*d / (vSegment.x*vSegment.x + vSegment.y*vSegment.y);
    
    return !(qAbs(inclineSinSq) > inclineThresholdSinSq);
}

bool OsmAnd::AtlasMapRendererSymbolsStage::tryToFindSpaceOnPath(
    const unsigned int pathSize,
    const QVector<glm::vec2>& pathInWorld,
    const QVector<glm::vec2>& pathOnScreen,
    const float requestedLengthInPixels,
    const unsigned int startPointIndex,
    const bool alreadyOccupiedLengthIsIn2D,
    const float alreadyOccupiedLength_,
    unsigned int& outEndPointIndex,
    bool& outLastOccupiedLengthIsIn2D,
    float& outLastOccupiedLength) const
{
    const auto& internalState = getInternalState();

    unsigned int testPointIndex = startPointIndex + 1;
    bool isRenderableAs2D = alreadyOccupiedLengthIsIn2D;
    auto requestedLength = requestedLengthInPixels * (isRenderableAs2D ? 1.0f : internalState.pixelInWorldProjectionScale);
    float entireSegmentLength = 0.0f;
    float alreadyOccupiedLength = alreadyOccupiedLength_;
    while (testPointIndex < pathSize)
    {
        // Check if last segment [testPointIndex - 1, testPointIndex] can be 2D.
        // This check is only performed if previously was renderable as 2D, since after test fails there's no way to go back to 2D
        if (isRenderableAs2D)
        {
            isRenderableAs2D = isInclineAllowedFor2D(pathOnScreen[testPointIndex - 1], pathOnScreen[testPointIndex]);
            if (!isRenderableAs2D)
            {
                // Recalculate alreadyOccupiedLength from 2D to 3D length in segment [startPointIdx, startPointIdx + 1]
                if (alreadyOccupiedLength > 0.0f)
                {
                    const auto firstLength = glm::distance(
                        pathOnScreen[startPointIndex],
                        pathOnScreen[startPointIndex + 1]);
                    const auto alreadyOccupiedPortion = alreadyOccupiedLength / firstLength;

                    alreadyOccupiedLength = glm::distance(
                        pathInWorld[startPointIndex],
                        pathInWorld[startPointIndex + 1]) * alreadyOccupiedPortion;
                }

                // Recalculate entire length of segment [startPointIdx, testPointIdx - 1] as 3D
                entireSegmentLength = 0.0f;
                requestedLength *= internalState.pixelInWorldProjectionScale;
                for (auto idx = startPointIndex + 1; idx < testPointIndex; idx++)
                {
                    const auto currentSegmentLength = glm::distance(pathInWorld[idx - 1], pathInWorld[idx]);
                    entireSegmentLength += currentSegmentLength;

                    // Also perform check if entire segment can fit the requested length, since what doesn't fit in 2D may fit in 3D
                    if (entireSegmentLength - alreadyOccupiedLength >= requestedLength)
                    {
                        outEndPointIndex = idx;
                        outLastOccupiedLengthIsIn2D = false;
                        outLastOccupiedLength = currentSegmentLength - (entireSegmentLength - alreadyOccupiedLength - requestedLength);
                        assert(outLastOccupiedLength <= currentSegmentLength);
                        assert(outLastOccupiedLength >= 0.0f);

                        return true;
                    }
                }
            }
        }

        // Add length of last segment [testPointIndex - 1, testPointIndex]
        const auto currentSegmentLength = glm::distance(
            (isRenderableAs2D ? pathOnScreen : pathInWorld)[testPointIndex - 1],
            (isRenderableAs2D ? pathOnScreen : pathInWorld)[testPointIndex]);
        entireSegmentLength += currentSegmentLength;

        // If segment length with subtracted "already-occupied portion" can fit requested length,
        // then plot a renderable there
        if (entireSegmentLength - alreadyOccupiedLength >= requestedLength)
        {
            outEndPointIndex = testPointIndex;
            outLastOccupiedLengthIsIn2D = isRenderableAs2D;
            outLastOccupiedLength = currentSegmentLength - (entireSegmentLength - alreadyOccupiedLength - requestedLength);
            assert(outLastOccupiedLength <= currentSegmentLength);
            assert(outLastOccupiedLength >= 0.0f);

            return true;
        }

        // Move to next text point
        testPointIndex++;
    }

    return false;
}

glm::vec2 OsmAnd::AtlasMapRendererSymbolsStage::computeSubpathDirection(
    const QVector<glm::vec2>& path,
    const unsigned int startPointIndex,
    const unsigned int endPointIndex) const
{
    glm::vec2 subpathDirection;
    auto pPrevPoint = path.constData() + startPointIndex;
    auto pPoint = pPrevPoint + 1;
    for (auto idx = startPointIndex + 1; idx <= endPointIndex; idx++)
        subpathDirection += (*(pPoint++) - *(pPrevPoint++));

    return glm::normalize(subpathDirection);
}

//QVector<OsmAnd::AtlasMapRendererSymbolsStage::RenderableOnPathSymbol::GlyphPlacement>
//OsmAnd::AtlasMapRendererSymbolsStage::computePlacementOfGlyphsOnPath(
//    const bool is2D,
//    const QVector<glm::vec2>& pathInWorld,
//    const QVector<glm::vec2>& pathOnScreen,
//    const unsigned int startPointIndex,
//    const unsigned int endPointIndex,
//    const float offsetFromStart) const
//{
//    const auto& internalState = getInternalState();
//
//    const auto& path = is2D ? pathOnScreen : pathInWorld;
//    //NOTE: Original algorithm for 3D SOPs contained a top-down projection that didn't include camera elevation angle. But this should give same results.
//    const auto scale = is2D ? 1.0f : internalState.pixelInWorldProjectionScale;
//    const auto symbolWidth = gpuResource->width*scale;
//    auto& length = renderable->subpathLength;
//    auto& segmentLengths = renderable->segmentLengths;
//    auto& offset = renderable->offset;
//
//    // Calculate offset for 'center' alignment and check if symbol can fit given path
//    auto pPrevPoint = path.constData();
//    auto pPoint = pPrevPoint + 1;
//    length = 0.0f;
//    const auto& pointsCount = path.size();
//    segmentLengths.resize(pointsCount - 1);
//    auto pSegmentLength = segmentLengths.data();
//    for (auto idx = 1; idx < pointsCount; idx++, pPoint++, pPrevPoint++)
//    {
//        const auto& distance = glm::distance(*pPoint, *pPrevPoint);
//        *(pSegmentLength++) = distance;
//        length += distance;
//    }
//    if (length < symbolWidth)
//    {
//        if (Q_UNLIKELY(debugSettings->showTooShortOnPathSymbolsRenderablesPaths))
//        {
//            QVector< glm::vec3 > debugPoints;
//            for (const auto& pointInWorld : renderable->subpathPointsInWorld)
//            {
//                debugPoints.push_back(qMove(glm::vec3(
//                    pointInWorld.x,
//                    0.0f,
//                    pointInWorld.y)));
//            }
//            getRenderer()->debugStage->addLine3D(debugPoints, SkColorSetA(is2D ? SK_ColorYELLOW : SK_ColorBLUE, 128));
//        }
//
//        // If length of path is not enough to contain entire symbol, remove this subpath entirely
//        itRenderableSOP.remove();
//        continue;
//    }
//    offset = (length - symbolWidth) / 2.0f;
//
//    // Adjust subpath start index to cut off unused segments
//    auto& startIndex = renderable->subpathStartIndex;
//    float lengthFromStart = 0.0f;
//    float prevLengthFromStart = 0.0f;
//    pSegmentLength = segmentLengths.data();
//    while (lengthFromStart <= offset)
//    {
//        prevLengthFromStart = lengthFromStart;
//        lengthFromStart += *(pSegmentLength++);
//        startIndex++;
//    }
//    startIndex--;
//    assert(startIndex >= 0);
//
//    // Adjust subpath end index to cut off unused segments
//    auto& endIndex = renderable->subpathEndIndex;
//    endIndex = startIndex + 1;
//    while (lengthFromStart <= offset + symbolWidth)
//    {
//        lengthFromStart += *(pSegmentLength++);
//        endIndex++;
//    }
//    assert(endIndex < pointsCount);
//    assert(endIndex - startIndex > 0);
//
//    // Adjust offset to reflect the changed
//    offset -= prevLengthFromStart;
//
//    //// Calculate direction of used subpath
//    //glm::vec2 subpathDirection;
//    //pPrevPoint = path.data() + startIndex;
//    //pPoint = pPrevPoint + 1;
//    //for (auto idx = startIndex + 1; idx <= endIndex; idx++, pPrevPoint++, pPoint++)
//    //    subpathDirection += (*pPoint - *pPrevPoint);
//
//    //if (is2D)
//    //    renderable->subpathDirectionOnScreen = glm::normalize(subpathDirection);
//    //else
//    //{
//    //    // For 3D SOPs direction is still threated as direction on screen
//    //    const auto& p0 = path.first();
//    //    const auto p1 = p0 + subpathDirection;
//    //    const auto& projectedP0 = glm::project(
//    //        glm::vec3(p0.x, 0.0f, p0.y),
//    //        internalState.mCameraView,
//    //        internalState.mPerspectiveProjection,
//    //        internalState.glmViewport);
//    //    const auto& projectedP1 = glm::project(
//    //        glm::vec3(p1.x, 0.0f, p1.y),
//    //        internalState.mCameraView,
//    //        internalState.mPerspectiveProjection,
//    //        internalState.glmViewport);
//    //    renderable->subpathDirectioInWorld = glm::normalize(p1 - p0);
//    //    renderable->subpathDirectionOnScreen = glm::normalize(projectedP1.xy - projectedP0.xy);
//    //}
//}
//
//void OsmAnd::AtlasMapRendererSymbolsStage::placeGlyphsOnPathSymbolSubpath(
//    const std::shared_ptr<RenderableOnPathSymbol>& renderable,
//    bool& outShouldInvert,
//    glm::vec2& outDirectionOnScreenN) const
//{
//    const auto& internalState = getInternalState();
//    const auto& symbol = std::static_pointer_cast<const OnPathMapSymbol>(renderable->mapSymbol);
//
//    const auto& is2D = renderable->is2D;
//    const auto& points = is2D ? renderable->subpathPointsOnScreen : renderable->subpathPointsInWorld;
//    //NOTE: Original algorithm for 3D SOPs contained a top-down projection that didn't include camera elevation angle. But this should give same results.
//    const auto projectionScale = is2D ? 1.0f : internalState.pixelInWorldProjectionScale;
//    const auto& subpathDirectionOnScreen = renderable->subpathDirectionOnScreen;
//    const glm::vec2 subpathDirectionOnScreenN(-subpathDirectionOnScreen.y, subpathDirectionOnScreen.x);
//    const auto shouldInvert = (subpathDirectionOnScreenN.y /* horizont.x*dirN.y - horizont.y*dirN.x == 1.0f*dirN.y - 0.0f*dirN.x */) < 0;
//    renderable->glyphsPlacement.reserve(symbol->glyphsWidth.size());
//
//    auto nextSubpathPointIdx = renderable->subpathStartIndex;
//    float lastSegmentLength = 0.0f;
//    glm::vec2 vLastPoint0;
//    glm::vec2 vLastPoint1;
//    glm::vec2 vLastSegment;
//    glm::vec2 vLastSegmentN;
//    float lastSegmentAngle = 0.0f;
//    float segmentsLengthSum = 0.0f;
//    float prevOffset = renderable->offset;
//    const auto glyphsCount = symbol->glyphsWidth.size();
//    auto pGlyphWidth = symbol->glyphsWidth.constData();
//    const auto glyphIterationDirection = shouldInvert ? -1 : +1;
//    if (shouldInvert)
//    {
//        // In case of direction inversion, start from last glyph
//        pGlyphWidth += (glyphsCount - 1);
//    }
//    for (int idx = 0; idx < glyphsCount; idx++, pGlyphWidth += glyphIterationDirection)
//    {
//        // Get current glyph anchor offset and provide offset for next glyph
//        const auto& glyphWidth = *pGlyphWidth;
//        const auto glyphWidthScaled = glyphWidth*projectionScale;
//        const auto anchorOffset = prevOffset + glyphWidthScaled / 2.0f;
//        prevOffset += glyphWidthScaled;
//
//        // Get subpath segment, where anchor is located
//        while (segmentsLengthSum < anchorOffset)
//        {
//            const auto& p0 = points[nextSubpathPointIdx];
//            assert(nextSubpathPointIdx + 1 <= renderable->subpathEndIndex);
//            const auto& p1 = points[nextSubpathPointIdx + 1];
//            lastSegmentLength = renderable->segmentLengths[nextSubpathPointIdx];
//            segmentsLengthSum += lastSegmentLength;
//            nextSubpathPointIdx++;
//
//            vLastPoint0.x = p0.x;
//            vLastPoint0.y = p0.y;
//            vLastPoint1.x = p1.x;
//            vLastPoint1.y = p1.y;
//            vLastSegment = (vLastPoint1 - vLastPoint0) / lastSegmentLength;
//            if (is2D)
//            {
//                // CCW 90 degrees rotation of Y is up
//                vLastSegmentN.x = -vLastSegment.y;
//                vLastSegmentN.y = vLastSegment.x;
//            }
//            else
//            {
//                // CCW 90 degrees rotation of Y is down
//                vLastSegmentN.x = vLastSegment.y;
//                vLastSegmentN.y = -vLastSegment.x;
//            }
//            lastSegmentAngle = qAtan2(vLastSegment.y, vLastSegment.x);//TODO: maybe for 3D a -y should be passed (see -1 rotation axis)
//            if (shouldInvert)
//                lastSegmentAngle = Utilities::normalizedAngleRadians(lastSegmentAngle + M_PI);
//        }
//
//        // Calculate anchor point
//        const auto anchorPoint = vLastPoint0 + (anchorOffset - (segmentsLengthSum - lastSegmentLength))*vLastSegment;
//
//        // Add glyph location data
//        RenderableOnPathSymbol::GlyphPlacement glyphLocation(anchorPoint, glyphWidth, lastSegmentAngle, vLastSegmentN);
//        if (shouldInvert)
//            renderable->glyphsPlacement.push_front(qMove(glyphLocation));
//        else
//            renderable->glyphsPlacement.push_back(qMove(glyphLocation));
//    }
//
//    outShouldInvert = shouldInvert;
//    outDirectionOnScreenN = subpathDirectionOnScreenN;
//}

double OsmAnd::AtlasMapRendererSymbolsStage::computeDistanceBetweenCameraToPath(
    const QVector<glm::vec2>& pathInWorld,
    const unsigned int startPointIndex,
    const unsigned int endPointIndex) const
{
    const auto& internalState = getInternalState();

    auto distanceToCamera = 0.0;
    for (const auto& pointInWorld : constOf(pathInWorld))
    {
        const auto& distance = glm::distance(internalState.worldCameraPosition, glm::vec3(pointInWorld.x, 0.0f, pointInWorld.y));
        if (distance > distanceToCamera)
            distanceToCamera = distance;
    }

    return distanceToCamera;
}

void OsmAnd::AtlasMapRendererSymbolsStage::sortRenderablesFromOnPathSymbols(
    const QList< std::shared_ptr<RenderableOnPathSymbol> >& entries,
    QMultiMap< float, std::shared_ptr<RenderableSymbol> >& output) const
{
    // Sort visible SOPs by distance to camera
    for (auto& renderable : entries)
    {
        // Insert into map
        const auto distanceToCamera = renderable->distanceToCamera;
        output.insert(distanceToCamera, qMove(renderable));
    }
}

void OsmAnd::AtlasMapRendererSymbolsStage::processBillboardSymbols(
    const MapRenderer::PublishedMapSymbols& input,
    QMultiMap< float, std::shared_ptr<RenderableSymbol> >& output) const
{
    obtainAndSortRenderablesFromBillboardSymbols(input, output);
}

void OsmAnd::AtlasMapRendererSymbolsStage::obtainAndSortRenderablesFromBillboardSymbols(
    const MapRenderer::PublishedMapSymbols& input,
    QMultiMap< float, std::shared_ptr<RenderableSymbol> >& output) const
{
    const auto& internalState = getInternalState();

    // Sort sprite symbols by distance to camera
    for (const auto& symbolEntry : rangeOf(constOf(input)))
    {
        const auto& symbol_ = symbolEntry.key();
        if (symbol_->isHidden)
            continue;
        const auto& symbol = std::dynamic_pointer_cast<const IBillboardMapSymbol>(symbol_);
        if (!symbol)
            continue;

        // Get GPU resource
        const auto gpuResource = captureGpuResource(symbolEntry.value(), symbol_);
        if (!gpuResource)
            continue;

        std::shared_ptr<RenderableBillboardSymbol> renderable(new RenderableBillboardSymbol());
        renderable->mapSymbol = symbol_;
        renderable->gpuResource = gpuResource;

        // Calculate location of symbol in world coordinates.
        renderable->offsetFromTarget31 = symbol->getPosition31() - currentState.target31;
        renderable->offsetFromTarget = Utilities::convert31toFloat(renderable->offsetFromTarget31, currentState.zoomBase);
        renderable->positionInWorld = glm::vec3(
            renderable->offsetFromTarget.x * AtlasMapRenderer::TileSize3D,
            0.0f,
            renderable->offsetFromTarget.y * AtlasMapRenderer::TileSize3D);

        // Get distance from symbol to camera
        const auto distanceToCamera = renderable->distanceToCamera = glm::distance(internalState.worldCameraPosition, renderable->positionInWorld);

        // Insert into map
        output.insert(distanceToCamera, qMove(renderable));
    }
}

void OsmAnd::AtlasMapRendererSymbolsStage::processOnSurfaceSymbols(
    const MapRenderer::PublishedMapSymbols& input,
    QMultiMap< float, std::shared_ptr<RenderableSymbol> >& output) const
{
    obtainAndSortRenderablesFromOnSurfaceSymbols(input, output);
}

void OsmAnd::AtlasMapRendererSymbolsStage::obtainAndSortRenderablesFromOnSurfaceSymbols(
    const MapRenderer::PublishedMapSymbols& input,
    QMultiMap< float, std::shared_ptr<RenderableSymbol> >& output) const
{
    const auto& internalState = getInternalState();

    // Sort on-surface symbols by distance to camera
    for (const auto& symbolEntry : rangeOf(constOf(input)))
    {
        const auto& symbol_ = symbolEntry.key();
        if (symbol_->isHidden)
            continue;
        const auto& symbol = std::dynamic_pointer_cast<const IOnSurfaceMapSymbol>(symbol_);
        if (!symbol)
            continue;

        // Get GPU resource
        const auto gpuResource = captureGpuResource(symbolEntry.value(), symbol_);
        if (!gpuResource)
            continue;

        std::shared_ptr<RenderableOnSurfaceSymbol> renderable(new RenderableOnSurfaceSymbol());
        renderable->mapSymbol = symbol_;
        renderable->gpuResource = gpuResource;

        // Calculate location of symbol in world coordinates.
        renderable->offsetFromTarget31 = symbol->getPosition31() - currentState.target31;
        renderable->offsetFromTarget = Utilities::convert31toFloat(renderable->offsetFromTarget31, currentState.zoomBase);
        renderable->positionInWorld = glm::vec3(
            renderable->offsetFromTarget.x * AtlasMapRenderer::TileSize3D,
            0.0f,
            renderable->offsetFromTarget.y * AtlasMapRenderer::TileSize3D);

        // Get direction
        if (symbol->isAzimuthAlignedDirection())
            renderable->direction = Utilities::normalizedAngleDegrees(currentState.azimuth + 180.0f);
        else
            renderable->direction = symbol->getDirection();

        // Get distance from symbol to camera
        const auto distanceToCamera = renderable->distanceToCamera = glm::distance(internalState.worldCameraPosition, renderable->positionInWorld);

        // Insert into map
        output.insert(distanceToCamera, qMove(renderable));
    }
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotBillboardSymbol(
    const std::shared_ptr<RenderableBillboardSymbol>& renderable,
    IntersectionsQuadTree& intersections) const
{
    if (std::dynamic_pointer_cast<const RasterMapSymbol>(renderable->mapSymbol))
    {
        return plotBillboardRasterSymbol(
            renderable,
            intersections);
    }
    else if (std::dynamic_pointer_cast<const VectorMapSymbol>(renderable->mapSymbol))
    {
        return plotBillboardVectorSymbol(
            renderable,
            intersections);
    }

    assert(false);
    return false;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotBillboardRasterSymbol(
    const std::shared_ptr<RenderableBillboardSymbol>& renderable,
    IntersectionsQuadTree& intersections) const
{
    const auto& internalState = getInternalState();

    const auto& symbol = std::static_pointer_cast<const BillboardRasterMapSymbol>(renderable->mapSymbol);
    const auto& symbolGroupPtr = symbol->groupPtr;

    // Calculate position in screen coordinates (same calculation as done in shader)
    const auto symbolOnScreen = glm::project(
        renderable->positionInWorld,
        internalState.mCameraView,
        internalState.mPerspectiveProjection,
        internalState.glmViewport);

    // Get bounds in screen coordinates
    auto boundsInWindow = AreaI::fromCenterAndSize(
        static_cast<int>(symbolOnScreen.x + symbol->offset.x), static_cast<int>((currentState.windowSize.y - symbolOnScreen.y) + symbol->offset.y),
        symbol->size.x, symbol->size.y);
    //TODO: use symbolExtraTopSpace & symbolExtraBottomSpace from font via Rasterizer_P
//    boundsInWindow.enlargeBy(PointI(3.0f*setupOptions.displayDensityFactor, 10.0f*setupOptions.displayDensityFactor)); /* 3dip; 10dip */

    if (!applyIntersectionWithOtherSymbolsFiltering(boundsInWindow, symbol, intersections))
        return false;

    if (!applyMinDistanceToSameContentFromOtherSymbolFiltering(boundsInWindow, symbol, intersections))
        return false;

    return plotSymbol(boundsInWindow, symbol, intersections);
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotBillboardVectorSymbol(
    const std::shared_ptr<RenderableBillboardSymbol>& renderable,
    IntersectionsQuadTree& intersections) const
{
    assert(false);
    return false;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotOnPathSymbol(
    const std::shared_ptr<RenderableOnPathSymbol>& renderable,
    IntersectionsQuadTree& intersections) const
{
    const auto& internalState = getInternalState();

    const auto& symbol = std::static_pointer_cast<const OnPathMapSymbol>(renderable->mapSymbol);
    const auto& symbolGroupPtr = symbol->groupPtr;

    // Draw the glyphs
    if (renderable->is2D)
    {
        // Calculate OOBB for 2D SOP
        const auto oobb = calculateOnPath2dOOBB(renderable);

        //TODO: use symbolExtraTopSpace & symbolExtraBottomSpace from font via Rasterizer_P
//        oobb.enlargeBy(PointF(3.0f*setupOptions.displayDensityFactor, 10.0f*setupOptions.displayDensityFactor)); /* 3dip; 10dip */

        if (!applyIntersectionWithOtherSymbolsFiltering(oobb, symbol, intersections))
            return false;

        if (!applyMinDistanceToSameContentFromOtherSymbolFiltering(oobb, symbol, intersections))
            return false;

        if (!plotSymbol(oobb, symbol, intersections))
            return false;

        if (Q_UNLIKELY(debugSettings->showOnPath2dSymbolGlyphDetails))
        {
            for (const auto& glyph : constOf(renderable->glyphsPlacement))
            {
                getRenderer()->debugStage->addRect2D(AreaF::fromCenterAndSize(
                    glyph.anchorPoint.x, currentState.windowSize.y - glyph.anchorPoint.y,
                    glyph.width, symbol->size.y), SkColorSetA(SK_ColorGREEN, 128), glyph.angle);

                QVector<glm::vec2> lineN;
                const auto ln0 = glyph.anchorPoint;
                lineN.push_back(glm::vec2(ln0.x, currentState.windowSize.y - ln0.y));
                const auto ln1 = glyph.anchorPoint + (glyph.vNormal*16.0f);
                lineN.push_back(glm::vec2(ln1.x, currentState.windowSize.y - ln1.y));
                getRenderer()->debugStage->addLine2D(lineN, SkColorSetA(SK_ColorMAGENTA, 128));
            }
        }
    }
    else
    {
        // Calculate OOBB for 3D SOP in world
        const auto oobb = calculateOnPath3dOOBB(renderable);

        //TODO: use symbolExtraTopSpace & symbolExtraBottomSpace from font via Rasterizer_P
//        oobb.enlargeBy(PointF(3.0f*setupOptions.displayDensityFactor, 10.0f*setupOptions.displayDensityFactor)); /* 3dip; 10dip */

        if (!applyIntersectionWithOtherSymbolsFiltering(oobb, symbol, intersections))
            return false;

        if (!applyMinDistanceToSameContentFromOtherSymbolFiltering(oobb, symbol, intersections))
            return false;

        if (!plotSymbol(oobb, symbol, intersections))
            return false;

        if (Q_UNLIKELY(debugSettings->showOnPath3dSymbolGlyphDetails))
        {
            for (const auto& glyph : constOf(renderable->glyphsPlacement))
            {
                const auto& glyphInMapPlane = AreaF::fromCenterAndSize(
                    glyph.anchorPoint.x, glyph.anchorPoint.y, /* anchor points are specified in world coordinates already */
                    glyph.width*internalState.pixelInWorldProjectionScale, symbol->size.y*internalState.pixelInWorldProjectionScale);
                const auto& tl = glyphInMapPlane.topLeft;
                const auto& tr = glyphInMapPlane.topRight();
                const auto& br = glyphInMapPlane.bottomRight;
                const auto& bl = glyphInMapPlane.bottomLeft();
                const glm::vec3 pC(glyph.anchorPoint.x, 0.0f, glyph.anchorPoint.y);
                const glm::vec4 p0(tl.x, 0.0f, tl.y, 1.0f);
                const glm::vec4 p1(tr.x, 0.0f, tr.y, 1.0f);
                const glm::vec4 p2(br.x, 0.0f, br.y, 1.0f);
                const glm::vec4 p3(bl.x, 0.0f, bl.y, 1.0f);
                const auto toCenter = glm::translate(-pC);
                const auto rotate = glm::rotate(qRadiansToDegrees((float)Utilities::normalizedAngleRadians(glyph.angle + M_PI)), glm::vec3(0.0f, -1.0f, 0.0f));
                const auto fromCenter = glm::translate(pC);
                const auto M = fromCenter*rotate*toCenter;
                getRenderer()->debugStage->addQuad3D((M*p0).xyz, (M*p1).xyz, (M*p2).xyz, (M*p3).xyz, SkColorSetA(SK_ColorGREEN, 128));

                QVector<glm::vec3> lineN;
                const auto ln0 = glyph.anchorPoint;
                lineN.push_back(glm::vec3(ln0.x, 0.0f, ln0.y));
                const auto ln1 = glyph.anchorPoint + (glyph.vNormal*16.0f*internalState.pixelInWorldProjectionScale);
                lineN.push_back(glm::vec3(ln1.x, 0.0f, ln1.y));
                getRenderer()->debugStage->addLine3D(lineN, SkColorSetA(SK_ColorMAGENTA, 128));
            }
        }
    }

    return true;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotOnSurfaceSymbol(
    const std::shared_ptr<RenderableOnSurfaceSymbol>& renderable,
    IntersectionsQuadTree& intersections) const
{
    if (std::dynamic_pointer_cast<const RasterMapSymbol>(renderable->mapSymbol))
    {
        return plotOnSurfaceRasterSymbol(
            renderable,
            intersections);
    }
    else if (std::dynamic_pointer_cast<const VectorMapSymbol>(renderable->mapSymbol))
    {
        return plotOnSurfaceVectorSymbol(
            renderable,
            intersections);
    }

    assert(false);
    return false;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotOnSurfaceRasterSymbol(
    const std::shared_ptr<RenderableOnSurfaceSymbol>& renderable,
    IntersectionsQuadTree& intersections) const
{
    const auto& internalState = getInternalState();

    const auto& symbol = std::static_pointer_cast<const OnSurfaceRasterMapSymbol>(renderable->mapSymbol);
    const auto& symbolGroupPtr = symbol->groupPtr;

    return true;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotOnSurfaceVectorSymbol(
    const std::shared_ptr<RenderableOnSurfaceSymbol>& renderable,
    IntersectionsQuadTree& intersections) const
{
    const auto& internalState = getInternalState();

    const auto& symbol = std::static_pointer_cast<const OnSurfaceVectorMapSymbol>(renderable->mapSymbol);
    const auto& symbolGroupPtr = symbol->groupPtr;
    
    return true;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::applyIntersectionWithOtherSymbolsFiltering(
    const AreaI boundsInWindow,
    const std::shared_ptr<const MapSymbol>& symbol,
    const IntersectionsQuadTree& intersections) const
{
    if (symbol->intersectionModeFlags & MapSymbol::IgnoredByIntersectionTest)
        return true;

    if (Q_UNLIKELY(debugSettings->skipSymbolsIntersectionCheck))
        return true;
    
    // Check intersections
    const auto symbolGroupPtr = symbol->groupPtr;
    const auto intersects = intersections.test(boundsInWindow, false,
        [symbolGroupPtr]
        (const std::shared_ptr<const MapSymbol>& otherSymbol, const IntersectionsQuadTree::BBox& otherBBox) -> bool
        {
            // Only accept intersections with symbols from other groups
            return otherSymbol->groupPtr != symbolGroupPtr;
        });
    if (intersects)
    {
        if (Q_UNLIKELY(debugSettings->showSymbolsBBoxesRejectedByIntersectionCheck))
            getRenderer()->debugStage->addRect2D((AreaF)boundsInWindow, SkColorSetA(SK_ColorRED, 50));
        return false;
    }
   
    return true;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::applyIntersectionWithOtherSymbolsFiltering(
    const OOBBF oobb,
    const std::shared_ptr<const MapSymbol>& symbol,
    const IntersectionsQuadTree& intersections) const
{
    if (symbol->intersectionModeFlags & MapSymbol::IgnoredByIntersectionTest)
        return true;

    if (Q_UNLIKELY(debugSettings->skipSymbolsIntersectionCheck))
        return true;

    // Check intersections
    const auto symbolGroupPtr = symbol->groupPtr;
    const auto intersects = intersections.test(OOBBI(oobb), false,
        [symbolGroupPtr]
        (const std::shared_ptr<const MapSymbol>& otherSymbol, const IntersectionsQuadTree::BBox& otherBBox) -> bool
        {
            // Only accept intersections with symbols from other groups
            return otherSymbol->groupPtr != symbolGroupPtr;
        });
    if (intersects)
    {
        if (Q_UNLIKELY(debugSettings->showSymbolsBBoxesRejectedByIntersectionCheck))
            getRenderer()->debugStage->addRect2D(oobb.unrotatedBBox, SkColorSetA(SK_ColorRED, 50), oobb.rotation);
        return false;
    }
    
    return true;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::applyMinDistanceToSameContentFromOtherSymbolFiltering(
    const AreaI boundsInWindow,
    const std::shared_ptr<const RasterMapSymbol>& symbol,
    const IntersectionsQuadTree& intersections) const
{
    if ((symbol->minDistance.x <= 0 && symbol->minDistance.y <= 0) || symbol->content.isNull())
        return true;

    if (Q_UNLIKELY(debugSettings->skipSymbolsMinDistanceToSameContentFromOtherSymbolCheck))
        return true;

    // Query for similar content in area of "minDistance" to exclude duplicates, but keep if from same mapObject
    const auto symbolGroupPtr = symbol->groupPtr;
    const auto& symbolContent = symbol->content;
    const auto hasSimilarContent = intersections.test(boundsInWindow.getEnlargedBy(symbol->minDistance), false,
        [symbolContent, symbolGroupPtr]
        (const std::shared_ptr<const MapSymbol>& otherSymbol_, const IntersectionsQuadTree::BBox& otherBBox) -> bool
        {
            const auto otherSymbol = std::dynamic_pointer_cast<const RasterMapSymbol>(otherSymbol_);
            if (!otherSymbol)
                return false;

            return
                (otherSymbol->groupPtr != symbolGroupPtr) &&
                (otherSymbol->content == symbolContent);
        });
    if (hasSimilarContent)
    {
        if (Q_UNLIKELY(debugSettings->showSymbolsBBoxesRejectedByMinDistanceToSameContentFromOtherSymbolCheck))
        {
            getRenderer()->debugStage->addRect2D(AreaF(boundsInWindow.getEnlargedBy(symbol->minDistance)), SkColorSetA(SK_ColorRED, 50));
            getRenderer()->debugStage->addRect2D(AreaF(boundsInWindow), SkColorSetA(SK_ColorRED, 128));
        }
        return false;
    }

    return true;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::applyMinDistanceToSameContentFromOtherSymbolFiltering(
    const OOBBF oobb,
    const std::shared_ptr<const RasterMapSymbol>& symbol,
    const IntersectionsQuadTree& intersections) const
{
    if ((symbol->minDistance.x <= 0 && symbol->minDistance.y <= 0) || symbol->content.isNull())
        return true;

    if (Q_UNLIKELY(debugSettings->skipSymbolsMinDistanceToSameContentFromOtherSymbolCheck))
        return true;

    // Query for similar content in area of "minDistance" to exclude duplicates, but keep if from same mapObject
    const auto symbolGroupPtr = symbol->groupPtr;
    const auto& symbolContent = symbol->content;
    const auto hasSimilarContent = intersections.test(OOBBI(oobb).getEnlargedBy(symbol->minDistance), false,
        [symbolContent, symbolGroupPtr]
        (const std::shared_ptr<const MapSymbol>& otherSymbol_, const IntersectionsQuadTree::BBox& otherBBox) -> bool
        {
            const auto otherSymbol = std::dynamic_pointer_cast<const RasterMapSymbol>(otherSymbol_);
            if (!otherSymbol)
                return false;

            return
                (otherSymbol->groupPtr != symbolGroupPtr) &&
                (otherSymbol->content == symbolContent);
        });
    if (hasSimilarContent)
    {
        if (Q_UNLIKELY(debugSettings->showSymbolsBBoxesRejectedByMinDistanceToSameContentFromOtherSymbolCheck))
        {
            getRenderer()->debugStage->addRect2D(oobb.getEnlargedBy(PointF(symbol->minDistance)).unrotatedBBox, SkColorSetA(SK_ColorRED, 50), oobb.rotation);
            getRenderer()->debugStage->addRect2D(oobb.unrotatedBBox, SkColorSetA(SK_ColorRED, 128), oobb.rotation);
        }
        return false;
    }

    return true;
}

OsmAnd::OOBBF OsmAnd::AtlasMapRendererSymbolsStage::calculateOnPath2dOOBB(const std::shared_ptr<RenderableOnPathSymbol>& renderable) const
{
    const auto& symbol = std::static_pointer_cast<const OnPathMapSymbol>(renderable->mapSymbol);

    const auto directionAngle = qAtan2(renderable->directionOnScreen.y, renderable->directionOnScreen.x);
    const auto negDirectionAngleCos = qCos(-directionAngle);
    const auto negDirectionAngleSin = qSin(-directionAngle);
    const auto directionAngleCos = qCos(directionAngle);
    const auto directionAngleSin = qSin(directionAngle);
    const auto halfGlyphHeight = symbol->size.y / 2.0f;
    auto bboxInitialized = false;
    AreaF bboxInDirection;
    for (const auto& glyph : constOf(renderable->glyphsPlacement))
    {
        const auto halfGlyphWidth = glyph.width / 2.0f;
        const glm::vec2 glyphPoints[4] =
        {
            glm::vec2(-halfGlyphWidth, -halfGlyphHeight), // TL
            glm::vec2( halfGlyphWidth, -halfGlyphHeight), // TR
            glm::vec2( halfGlyphWidth,  halfGlyphHeight), // BR
            glm::vec2(-halfGlyphWidth,  halfGlyphHeight)  // BL
        };

        const auto segmentAngleCos = qCos(glyph.angle);
        const auto segmentAngleSin = qSin(glyph.angle);

        for (int idx = 0; idx < 4; idx++)
        {
            const auto& glyphPoint = glyphPoints[idx];

            // Rotate to align with it's segment
            glm::vec2 pointOnScreen;
            pointOnScreen.x = glyphPoint.x*segmentAngleCos - glyphPoint.y*segmentAngleSin;
            pointOnScreen.y = glyphPoint.x*segmentAngleSin + glyphPoint.y*segmentAngleCos;

            // Add anchor point
            pointOnScreen += glyph.anchorPoint;

            // Rotate to align with direction
            PointF alignedPoint;
            alignedPoint.x = pointOnScreen.x*negDirectionAngleCos - pointOnScreen.y*negDirectionAngleSin;
            alignedPoint.y = pointOnScreen.x*negDirectionAngleSin + pointOnScreen.y*negDirectionAngleCos;
            if (Q_LIKELY(bboxInitialized))
                bboxInDirection.enlargeToInclude(alignedPoint);
            else
            {
                bboxInDirection.topLeft = bboxInDirection.bottomRight = alignedPoint;
                bboxInitialized = true;
            }
        }
    }
    const auto alignedCenter = bboxInDirection.center();
    bboxInDirection -= alignedCenter;
    PointF centerOnScreen;
    centerOnScreen.x = alignedCenter.x*directionAngleCos - alignedCenter.y*directionAngleSin;
    centerOnScreen.y = alignedCenter.x*directionAngleSin + alignedCenter.y*directionAngleCos;
    bboxInDirection = AreaF::fromCenterAndSize(
        centerOnScreen.x, currentState.windowSize.y - centerOnScreen.y,
        bboxInDirection.width(), bboxInDirection.height());
    OOBBF oobb(bboxInDirection, directionAngle);

    return oobb;
}

OsmAnd::OOBBF OsmAnd::AtlasMapRendererSymbolsStage::calculateOnPath3dOOBB(const std::shared_ptr<RenderableOnPathSymbol>& renderable) const
{
    const auto& internalState = getInternalState();
    const auto& symbol = std::static_pointer_cast<const OnPathMapSymbol>(renderable->mapSymbol);

    const auto directionAngleInWorld = qAtan2(renderable->directionInWorld.y, renderable->directionInWorld.x);
    const auto negDirectionAngleInWorldCos = qCos(-directionAngleInWorld);
    const auto negDirectionAngleInWorldSin = qSin(-directionAngleInWorld);
    const auto directionAngleInWorldCos = qCos(directionAngleInWorld);
    const auto directionAngleInWorldSin = qSin(directionAngleInWorld);
    const auto halfGlyphHeight = (symbol->size.y / 2.0f) * internalState.pixelInWorldProjectionScale;
    auto bboxInWorldInitialized = false;
    AreaF bboxInWorldDirection;
    for (const auto& glyph : constOf(renderable->glyphsPlacement))
    {
        const auto halfGlyphWidth = (glyph.width / 2.0f) * internalState.pixelInWorldProjectionScale;
        const glm::vec2 glyphPoints[4] =
        {
            glm::vec2(-halfGlyphWidth, -halfGlyphHeight), // TL
            glm::vec2( halfGlyphWidth, -halfGlyphHeight), // TR
            glm::vec2( halfGlyphWidth,  halfGlyphHeight), // BR
            glm::vec2(-halfGlyphWidth,  halfGlyphHeight)  // BL
        };

        const auto segmentAngleCos = qCos(glyph.angle);
        const auto segmentAngleSin = qSin(glyph.angle);

        for (int idx = 0; idx < 4; idx++)
        {
            const auto& glyphPoint = glyphPoints[idx];

            // Rotate to align with it's segment
            glm::vec2 pointInWorld;
            pointInWorld.x = glyphPoint.x*segmentAngleCos - glyphPoint.y*segmentAngleSin;
            pointInWorld.y = glyphPoint.x*segmentAngleSin + glyphPoint.y*segmentAngleCos;

            // Add anchor point
            pointInWorld += glyph.anchorPoint;

            // Rotate to align with direction
            PointF alignedPoint;
            alignedPoint.x = pointInWorld.x*negDirectionAngleInWorldCos - pointInWorld.y*negDirectionAngleInWorldSin;
            alignedPoint.y = pointInWorld.x*negDirectionAngleInWorldSin + pointInWorld.y*negDirectionAngleInWorldCos;
            if (Q_LIKELY(bboxInWorldInitialized))
                bboxInWorldDirection.enlargeToInclude(alignedPoint);
            else
            {
                bboxInWorldDirection.topLeft = bboxInWorldDirection.bottomRight = alignedPoint;
                bboxInWorldInitialized = true;
            }
        }
    }
    const auto alignedCenterInWorld = bboxInWorldDirection.center();
    bboxInWorldDirection -= alignedCenterInWorld;

    PointF rotatedBBoxInWorld[4];
    const auto& tl = bboxInWorldDirection.topLeft;
    rotatedBBoxInWorld[0].x = tl.x*directionAngleInWorldCos - tl.y*directionAngleInWorldSin;
    rotatedBBoxInWorld[0].y = tl.x*directionAngleInWorldSin + tl.y*directionAngleInWorldCos;
    const auto& tr = bboxInWorldDirection.topRight();
    rotatedBBoxInWorld[1].x = tr.x*directionAngleInWorldCos - tr.y*directionAngleInWorldSin;
    rotatedBBoxInWorld[1].y = tr.x*directionAngleInWorldSin + tr.y*directionAngleInWorldCos;
    const auto& br = bboxInWorldDirection.bottomRight;
    rotatedBBoxInWorld[2].x = br.x*directionAngleInWorldCos - br.y*directionAngleInWorldSin;
    rotatedBBoxInWorld[2].y = br.x*directionAngleInWorldSin + br.y*directionAngleInWorldCos;
    const auto& bl = bboxInWorldDirection.bottomLeft();
    rotatedBBoxInWorld[3].x = bl.x*directionAngleInWorldCos - bl.y*directionAngleInWorldSin;
    rotatedBBoxInWorld[3].y = bl.x*directionAngleInWorldSin + bl.y*directionAngleInWorldCos;

    PointF centerInWorld;
    centerInWorld.x = alignedCenterInWorld.x*directionAngleInWorldCos - alignedCenterInWorld.y*directionAngleInWorldSin;
    centerInWorld.y = alignedCenterInWorld.x*directionAngleInWorldSin + alignedCenterInWorld.y*directionAngleInWorldCos;
    bboxInWorldDirection += centerInWorld;
    rotatedBBoxInWorld[0] += centerInWorld;
    rotatedBBoxInWorld[1] += centerInWorld;
    rotatedBBoxInWorld[2] += centerInWorld;
    rotatedBBoxInWorld[3] += centerInWorld;

#if OSMAND_DEBUG && 0
    {
        const auto& cc = bboxInWorldDirection.center();
        const auto& tl = bboxInWorldDirection.topLeft;
        const auto& tr = bboxInWorldDirection.topRight();
        const auto& br = bboxInWorldDirection.bottomRight;
        const auto& bl = bboxInWorldDirection.bottomLeft();

        const glm::vec3 pC(cc.x, 0.0f, cc.y);
        const glm::vec4 p0(tl.x, 0.0f, tl.y, 1.0f);
        const glm::vec4 p1(tr.x, 0.0f, tr.y, 1.0f);
        const glm::vec4 p2(br.x, 0.0f, br.y, 1.0f);
        const glm::vec4 p3(bl.x, 0.0f, bl.y, 1.0f);
        const auto toCenter = glm::translate(-pC);
        const auto rotate = glm::rotate(qRadiansToDegrees((float)Utilities::normalizedAngleRadians(directionAngleInWorld + M_PI)), glm::vec3(0.0f, -1.0f, 0.0f));
        const auto fromCenter = glm::translate(pC);
        const auto M = fromCenter*rotate*toCenter;
        getRenderer()->debugStage->addQuad3D((M*p0).xyz, (M*p1).xyz, (M*p2).xyz, (M*p3).xyz, SkColorSetA(SK_ColorGREEN, 50));
    }
#endif // OSMAND_DEBUG
#if OSMAND_DEBUG && 0
        {
            const auto& tl = rotatedBBoxInWorld[0];
            const auto& tr = rotatedBBoxInWorld[1];
            const auto& br = rotatedBBoxInWorld[2];
            const auto& bl = rotatedBBoxInWorld[3];

            const glm::vec3 p0(tl.x, 0.0f, tl.y);
            const glm::vec3 p1(tr.x, 0.0f, tr.y);
            const glm::vec3 p2(br.x, 0.0f, br.y);
            const glm::vec3 p3(bl.x, 0.0f, bl.y);
            getRenderer()->debugStage->addQuad3D(p0, p1, p2, p3, SkColorSetA(SK_ColorGREEN, 50));
        }
#endif // OSMAND_DEBUG

    // Project points of OOBB in world to screen
    const PointF projectedRotatedBBoxInWorldP0(static_cast<glm::vec2>(
        glm::project(glm::vec3(rotatedBBoxInWorld[0].x, 0.0f, rotatedBBoxInWorld[0].y),
        internalState.mCameraView,
        internalState.mPerspectiveProjection,
        internalState.glmViewport).xy));
    const PointF projectedRotatedBBoxInWorldP1(static_cast<glm::vec2>(
        glm::project(glm::vec3(rotatedBBoxInWorld[1].x, 0.0f, rotatedBBoxInWorld[1].y),
        internalState.mCameraView,
        internalState.mPerspectiveProjection,
        internalState.glmViewport).xy));
    const PointF projectedRotatedBBoxInWorldP2(static_cast<glm::vec2>(
        glm::project(glm::vec3(rotatedBBoxInWorld[2].x, 0.0f, rotatedBBoxInWorld[2].y),
        internalState.mCameraView,
        internalState.mPerspectiveProjection,
        internalState.glmViewport).xy));
    const PointF projectedRotatedBBoxInWorldP3(static_cast<glm::vec2>(
        glm::project(glm::vec3(rotatedBBoxInWorld[3].x, 0.0f, rotatedBBoxInWorld[3].y),
        internalState.mCameraView,
        internalState.mPerspectiveProjection,
        internalState.glmViewport).xy));
#if OSMAND_DEBUG && 0
    {
        QVector<glm::vec2> line;
        line.push_back(glm::vec2(projectedRotatedBBoxInWorldP0.x, currentState.windowSize.y - projectedRotatedBBoxInWorldP0.y));
        line.push_back(glm::vec2(projectedRotatedBBoxInWorldP1.x, currentState.windowSize.y - projectedRotatedBBoxInWorldP1.y));
        line.push_back(glm::vec2(projectedRotatedBBoxInWorldP2.x, currentState.windowSize.y - projectedRotatedBBoxInWorldP2.y));
        line.push_back(glm::vec2(projectedRotatedBBoxInWorldP3.x, currentState.windowSize.y - projectedRotatedBBoxInWorldP3.y));
        line.push_back(glm::vec2(projectedRotatedBBoxInWorldP0.x, currentState.windowSize.y - projectedRotatedBBoxInWorldP0.y));
        getRenderer()->debugStage->addLine2D(line, SkColorSetA(SK_ColorRED, 50));
    }
#endif // OSMAND_DEBUG

    // Rotate using direction on screen
    const auto directionAngle = qAtan2(renderable->directionOnScreen.y, renderable->directionOnScreen.x);
    const auto negDirectionAngleCos = qCos(-directionAngle);
    const auto negDirectionAngleSin = qSin(-directionAngle);
    PointF bboxOnScreenP0;
    bboxOnScreenP0.x = projectedRotatedBBoxInWorldP0.x*negDirectionAngleCos - projectedRotatedBBoxInWorldP0.y*negDirectionAngleSin;
    bboxOnScreenP0.y = projectedRotatedBBoxInWorldP0.x*negDirectionAngleSin + projectedRotatedBBoxInWorldP0.y*negDirectionAngleCos;
    PointF bboxOnScreenP1;
    bboxOnScreenP1.x = projectedRotatedBBoxInWorldP1.x*negDirectionAngleCos - projectedRotatedBBoxInWorldP1.y*negDirectionAngleSin;
    bboxOnScreenP1.y = projectedRotatedBBoxInWorldP1.x*negDirectionAngleSin + projectedRotatedBBoxInWorldP1.y*negDirectionAngleCos;
    PointF bboxOnScreenP2;
    bboxOnScreenP2.x = projectedRotatedBBoxInWorldP2.x*negDirectionAngleCos - projectedRotatedBBoxInWorldP2.y*negDirectionAngleSin;
    bboxOnScreenP2.y = projectedRotatedBBoxInWorldP2.x*negDirectionAngleSin + projectedRotatedBBoxInWorldP2.y*negDirectionAngleCos;
    PointF bboxOnScreenP3;
    bboxOnScreenP3.x = projectedRotatedBBoxInWorldP3.x*negDirectionAngleCos - projectedRotatedBBoxInWorldP3.y*negDirectionAngleSin;
    bboxOnScreenP3.y = projectedRotatedBBoxInWorldP3.x*negDirectionAngleSin + projectedRotatedBBoxInWorldP3.y*negDirectionAngleCos;

    // Build bbox from that and subtract center
    AreaF bboxInDirection;
    bboxInDirection.topLeft = bboxInDirection.bottomRight = bboxOnScreenP0;
    bboxInDirection.enlargeToInclude(bboxOnScreenP1);
    bboxInDirection.enlargeToInclude(bboxOnScreenP2);
    bboxInDirection.enlargeToInclude(bboxOnScreenP3);
    const auto alignedCenter = bboxInDirection.center();
    bboxInDirection -= alignedCenter;

    // Rotate center and add it
    const auto directionAngleCos = qCos(directionAngle);
    const auto directionAngleSin = qSin(directionAngle);
    PointF centerOnScreen;
    centerOnScreen.x = alignedCenter.x*directionAngleCos - alignedCenter.y*directionAngleSin;
    centerOnScreen.y = alignedCenter.x*directionAngleSin + alignedCenter.y*directionAngleCos;
    bboxInDirection = AreaF::fromCenterAndSize(
        centerOnScreen.x, currentState.windowSize.y - centerOnScreen.y,
        bboxInDirection.width(), bboxInDirection.height());
    OOBBF oobb(bboxInDirection, directionAngle);

    return oobb;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotSymbol(
    const AreaI boundsInWindow,
    const std::shared_ptr<const MapSymbol>& symbol,
    IntersectionsQuadTree& intersections) const
{
    if (!symbol->intersectionModeFlags.isSet(MapSymbol::TransparentForIntersectionLookup) &&
        !Q_UNLIKELY(debugSettings->allSymbolsTransparentForIntersectionLookup))
    {
        // Insert into quad-tree
        if (!intersections.insert(symbol, boundsInWindow))
        {
            if (debugSettings->showSymbolsBBoxesRejectedByIntersectionCheck)
                getRenderer()->debugStage->addRect2D((AreaF)boundsInWindow, SkColorSetA(SK_ColorBLUE, 50));
            return false;
        }
    }

    if (Q_UNLIKELY(debugSettings->showSymbolsBBoxesAcceptedByIntersectionCheck))
        getRenderer()->debugStage->addRect2D((AreaF)boundsInWindow, SkColorSetA(SK_ColorGREEN, 50));

    return true;
}

bool OsmAnd::AtlasMapRendererSymbolsStage::plotSymbol(
    const OOBBF oobb,
    const std::shared_ptr<const MapSymbol>& symbol,
    IntersectionsQuadTree& intersections) const
{
    if (!symbol->intersectionModeFlags.isSet(MapSymbol::TransparentForIntersectionLookup) &&
        !Q_UNLIKELY(debugSettings->allSymbolsTransparentForIntersectionLookup))
    {
        // Insert into quad-tree
        if (!intersections.insert(symbol, OOBBI(oobb)))
        {
            if (debugSettings->showSymbolsBBoxesRejectedByIntersectionCheck)
                getRenderer()->debugStage->addRect2D(oobb.unrotatedBBox, SkColorSetA(SK_ColorBLUE, 50), oobb.rotation);
            return false;
        }
    }

    if (Q_UNLIKELY(debugSettings->showSymbolsBBoxesAcceptedByIntersectionCheck))
        getRenderer()->debugStage->addRect2D(oobb.unrotatedBBox, SkColorSetA(SK_ColorGREEN, 50), oobb.rotation);

    return true;
}

std::shared_ptr<const OsmAnd::GPUAPI::ResourceInGPU> OsmAnd::AtlasMapRendererSymbolsStage::captureGpuResource(
    const MapRenderer::MapSymbolreferenceOrigins& resources,
    const std::shared_ptr<const MapSymbol>& mapSymbol)
{
    for (auto& resource : constOf(resources))
    {
        std::shared_ptr<const GPUAPI::ResourceInGPU> gpuResource;
        if (resource->setStateIf(MapRendererResourceState::Uploaded, MapRendererResourceState::IsBeingUsed))
        {
            if (const auto tiledResource = std::dynamic_pointer_cast<MapRendererTiledSymbolsResource>(resource))
                gpuResource = tiledResource->getGpuResourceFor(mapSymbol);
            else if (const auto keyedResource = std::dynamic_pointer_cast<MapRendererKeyedSymbolsResource>(resource))
                gpuResource = keyedResource->getGpuResourceFor(mapSymbol);

            resource->setState(MapRendererResourceState::Uploaded);
        }

        // Stop as soon as GPU resource found
        if (gpuResource)
            return gpuResource;
    }
    return nullptr;
}

OsmAnd::AtlasMapRendererSymbolsStage::RenderableSymbol::~RenderableSymbol()
{
}

OsmAnd::AtlasMapRendererSymbolsStage::RenderableBillboardSymbol::~RenderableBillboardSymbol()
{
}

OsmAnd::AtlasMapRendererSymbolsStage::RenderableOnPathSymbol::~RenderableOnPathSymbol()
{
}

OsmAnd::AtlasMapRendererSymbolsStage::RenderableOnSurfaceSymbol::~RenderableOnSurfaceSymbol()
{
}
