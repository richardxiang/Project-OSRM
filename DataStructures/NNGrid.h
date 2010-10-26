/*
    open source routing machine
    Copyright (C) Dennis Luxen, others 2010

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU AFFERO General Public License as published by
the Free Software Foundation; either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
or see http://www.gnu.org/licenses/agpl.txt.
 */

#ifndef NNGRID_H_
#define NNGRID_H_

#include <fstream>

#include <cassert>
#include <limits>
#include <cmath>
#include <vector>
#include <stxxl.h>
#include <google/dense_hash_map>

#include "ExtractorStructs.h"
#include "GridEdge.h"
#include "Percent.h"
#include "PhantomNodes.h"
#include "Util.h"

#include "StaticGraph.h"

namespace NNGrid{
static unsigned getFileIndexForLatLon(const int lt, const int ln)
{
    double lat = lt/100000.;
    double lon = ln/100000.;

    double x = ( lon + 180.0 ) / 360.0;
    double y = ( lat + 90.0 ) / 180.0;

    assert( x<=1.0 && x >= 0);
    assert( y<=1.0 && y >= 0);

    unsigned line = 1073741824.0*y;
    line = line - (line % 32768);
    assert(line % 32768 == 0);
    unsigned column = 32768.*x;
    unsigned fileIndex = line+column;
    return fileIndex;
}

static unsigned getRAMIndexFromFileIndex(const int fileIndex)
{
    unsigned fileLine = fileIndex / 32768;
    fileLine = fileLine / 32;
    fileLine = fileLine * 1024;
    unsigned fileColumn = (fileIndex % 32768);
    fileColumn = fileColumn / 32;
    unsigned ramIndex = fileLine + fileColumn;
    assert(ramIndex < 1024*1024);
    return ramIndex;
}

static inline int signum(int x){
    return (x > 0) ? 1 : (x < 0) ? -1 : 0;
}

static void bresenham(int xstart,int ystart,int xend,int yend, std::vector<std::pair<unsigned, unsigned> > &indexList)
{
    int x, y, t, dx, dy, incx, incy, pdx, pdy, ddx, ddy, es, el, err;

    dx = xend - xstart;
    dy = yend - ystart;

    incx = signum(dx);
    incy = signum(dy);
    if(dx<0) dx = -dx;
    if(dy<0) dy = -dy;

    if (dx>dy)
    {
        pdx=incx; pdy=0;
        ddx=incx; ddy=incy;
        es =dy;   el =dx;
    } else
    {
        pdx=0;    pdy=incy;
        ddx=incx; ddy=incy;
        es =dx;   el =dy;
    }
    x = xstart;
    y = ystart;
    err = el/2;
    {
        int fileIndex = (y-1)*32768 + x;
        int ramIndex = getRAMIndexFromFileIndex(fileIndex);
        indexList.push_back(std::make_pair(fileIndex, ramIndex));
    }

    for(t=0; t<el; ++t)
    {
        err -= es;
        if(err<0)
        {
            err += el;
            x += ddx;
            y += ddy;
        } else
        {
            x += pdx;
            y += pdy;
        }
        {
            int fileIndex = (y-1)*32768 + x;
            int ramIndex = getRAMIndexFromFileIndex(fileIndex);
            indexList.push_back(std::make_pair(fileIndex, ramIndex));
        }
    }
}

static void getListOfIndexesForEdgeAndGridSize(_Coordinate& start, _Coordinate& target, std::vector<std::pair<unsigned, unsigned> > &indexList)
{
    double lat1 = start.lat/100000.;
    double lon1 = start.lon/100000.;

    double x1 = ( lon1 + 180.0 ) / 360.0;
    double y1 = ( lat1 + 90.0 ) / 180.0;

    double lat2 = target.lat/100000.;
    double lon2 = target.lon/100000.;

    double x2 = ( lon2 + 180.0 ) / 360.0;
    double y2 = ( lat2 + 90.0 ) / 180.0;

    bresenham(x1*32768, y1*32768, x2*32768, y2*32768, indexList);
}

template<bool WriteAccess = false>
class NNGrid {

public:
    NNGrid() { ramIndexTable.resize((1024*1024), UINT_MAX); if( WriteAccess) { entries = new stxxl::vector<GridEdgeData>(); }}

    NNGrid(const char* rif, const char* iif) {
        ramIndexTable.resize((1024*1024), UINT_MAX);
        indexInFile.open(iif, std::ios::in | std::ios::binary);
        ramInFile.open(rif, std::ios::in | std::ios::binary);
    }

    ~NNGrid() {
        if(ramInFile.is_open()) ramInFile.close();
        if(indexInFile.is_open()) indexInFile.close();

        if (WriteAccess)
        {
            delete entries;
        }
    }

    void OpenIndexFiles()
    {
        assert(ramInFile.is_open());
        assert(indexInFile.is_open());

        for(int i = 0; i < 1024*1024; i++)
        {
            unsigned temp;
            ramInFile.read((char*)&temp, sizeof(unsigned));
            ramIndexTable[i] = temp;
        }
        ramInFile.close();
    }

    void AddEdge(_Edge edge, _Coordinate start, _Coordinate target)
    {
        edge.startCoord = start;
        edge.targetCoord = target;

        std::vector<std::pair<unsigned, unsigned> > indexList;
        getListOfIndexesForEdgeAndGridSize(start, target, indexList);
        for(int i = 0; i < indexList.size(); i++)
        {
            entries->push_back(GridEdgeData(edge, indexList[i].first, indexList[i].second));
        }
    }

    void ConstructGrid(char * ramIndexOut, char * fileIndexOut)
    {
        double timestamp = get_timestamp();
        //create index file on disk, old one is over written
        indexOutFile.open(fileIndexOut, std::ios::out | std::ios::binary | std::ios::trunc);
        cout << "sorting grid data consisting of " << entries->size() << " edges..." << flush;
        //sort entries
        stxxl::sort(entries->begin(), entries->end(), CompareGridEdgeDataByRamIndex(), 1024*1024*1024);
        cout << "ok in " << (get_timestamp() - timestamp) << "s" << endl;
        std::vector<GridEdgeData> entriesInFileWithRAMSameIndex;
        unsigned indexInRamTable = entries->begin()->ramIndex;
        unsigned lastPositionInIndexFile = 0;
        unsigned numberOfUsedCells = 0;
        unsigned maxNumberOfRAMCellElements = 0;
        cout << "writing data ..." << flush;
        Percent p(entries->size());
        for(stxxl::vector<GridEdgeData>::iterator vt = entries->begin(); vt != entries->end(); vt++)
        {
            p.printIncrement();
            if(vt->ramIndex != indexInRamTable)
            {
                unsigned numberOfBytesInCell = FillCell(entriesInFileWithRAMSameIndex, lastPositionInIndexFile);
                if(entriesInFileWithRAMSameIndex.size() > maxNumberOfRAMCellElements)
                    maxNumberOfRAMCellElements = entriesInFileWithRAMSameIndex.size();

                ramIndexTable[indexInRamTable] = lastPositionInIndexFile;
                lastPositionInIndexFile += numberOfBytesInCell;
                entriesInFileWithRAMSameIndex.clear();
                indexInRamTable = vt->ramIndex;
                numberOfUsedCells++;
            }
            entriesInFileWithRAMSameIndex.push_back(*vt);
        }
        unsigned numberOfBytesInCell = FillCell(entriesInFileWithRAMSameIndex, lastPositionInIndexFile);
        ramIndexTable[indexInRamTable] = lastPositionInIndexFile;
        numberOfUsedCells++;
        entriesInFileWithRAMSameIndex.clear();

        assert(entriesInFileWithRAMSameIndex.size() == 0);

        for(int i = 0; i < 1024*1024; i++)
        {
            if(ramIndexTable[i] != UINT_MAX){
                numberOfUsedCells--;
            }
        }
        assert(numberOfUsedCells == 0);

        //close index file
        indexOutFile.close();
        //Serialize RAM Index
        ofstream ramFile(ramIndexOut, std::ios::out | std::ios::binary | std::ios::trunc);
        //write 4 MB of index Table in RAM
        for(int i = 0; i < 1024*1024; i++)
            ramFile.write((char *)&ramIndexTable[i], sizeof(unsigned) );
        //close ram index file
        ramFile.close();
    }

    bool FindRoutingStarts(const _Coordinate startCoord, const _Coordinate targetCoord, PhantomNodes * routingStarts) {
        unsigned fileIndex = getFileIndexForLatLon(startCoord.lat, startCoord.lon);
        std::vector<_Edge> candidates;
        double timestamp = get_timestamp();
        for(int j = -32768; j < (32768+1); j+=32768){
            for(int i = -1; i < 2; i++){
                GetContentsOfFileBucket(fileIndex+i+j, candidates);
            }
        }

        _Coordinate tmp;
        double dist = numeric_limits<double>::max();
        timestamp = get_timestamp();
        for(std::vector<_Edge>::iterator it = candidates.begin(); it != candidates.end(); it++)
        {
            double r = 0.;
            double tmpDist = ComputeDistance(startCoord, it->startCoord, it->targetCoord, tmp, &r);
            if(tmpDist < dist)
            {
                routingStarts->startNode1 = it->start;
                routingStarts->startNode2 = it->target;
                routingStarts->startRatio = r;
                dist = tmpDist;
                routingStarts->startCoord.lat = tmp.lat;
                routingStarts->startCoord.lon = tmp.lon;
            }
        }

        fileIndex = getFileIndexForLatLon(targetCoord.lat, targetCoord.lon);
        candidates.clear();
        timestamp = get_timestamp();
        for(int j = -32768; j < (32768+1); j+=32768){
            for(int i = -1; i < 2; i++){
                GetContentsOfFileBucket(fileIndex+i+j, candidates);
            }
        }

        dist = numeric_limits<double>::max();
        timestamp = get_timestamp();
        for(std::vector<_Edge>::iterator it = candidates.begin(); it != candidates.end(); it++)
        {
            double r = 0.;
            double tmpDist = ComputeDistance(targetCoord, it->startCoord, it->targetCoord, tmp, &r);
            if(tmpDist < dist)
            {
                routingStarts->targetNode1 = it->start;
                routingStarts->targetNode2 = it->target;
                routingStarts->targetRatio = r;
                dist = tmpDist;
                routingStarts->targetCoord.lat = tmp.lat;
                routingStarts->targetCoord.lon = tmp.lon;
            }
        }
        return true;
    }

    _Coordinate FindNearestPointOnEdge(const _Coordinate& inputCoordinate)
    {
        unsigned fileIndex = getFileIndexForLatLon(inputCoordinate.lat, inputCoordinate.lon);
        std::vector<_Edge> candidates;
        double timestamp = get_timestamp();
        for(int j = -32768; j < (32768+1); j+=32768){
            for(int i = -1; i < 2; i++){
                GetContentsOfFileBucket(fileIndex+i+j, candidates);
            }
        }

        _Coordinate nearest(numeric_limits<int>::max(), numeric_limits<int>::max()), tmp;
        double dist = numeric_limits<double>::max();
        timestamp = get_timestamp();
        for(std::vector<_Edge>::iterator it = candidates.begin(); it != candidates.end(); it++)
        {
            double r = 0.;
            double tmpDist = ComputeDistance(inputCoordinate, it->startCoord, it->targetCoord, tmp, &r);
            if(tmpDist < dist)
            {
                dist = tmpDist;
                nearest = tmp;
            }
        }
        return nearest;
    }

private:
    unsigned FillCell(std::vector<GridEdgeData>& entriesWithSameRAMIndex, unsigned fileOffset )
    {
        vector<char> * tmpBuffer = new vector<char>();
        tmpBuffer->resize(32*32*4096,0);
        unsigned indexIntoTmpBuffer = 0;
        unsigned numberOfWrittenBytes = 0;
        assert(indexOutFile.is_open());

        vector<unsigned> cellIndex;
        cellIndex.resize(32*32,UINT_MAX);
        google::dense_hash_map< unsigned, unsigned > * cellMap = new google::dense_hash_map< unsigned, unsigned >(1024);
        cellMap->set_empty_key(UINT_MAX);

        unsigned ramIndex = entriesWithSameRAMIndex.begin()->ramIndex;
        unsigned lineBase = ramIndex/1024;
        lineBase = lineBase*32*32768;
        unsigned columnBase = ramIndex%1024;
        columnBase=columnBase*32;

        for(int i = 0; i < 32; i++)
        {
            for(int j = 0; j < 32; j++)
            {
                unsigned fileIndex = lineBase + i*32768 + columnBase+j;
                unsigned cellIndex = i*32+j;
                cellMap->insert(std::make_pair(fileIndex, cellIndex));
            }
        }

        for(int i = 0; i < entriesWithSameRAMIndex.size() -1; i++)
        {
            assert(entriesWithSameRAMIndex[i].ramIndex== entriesWithSameRAMIndex[i+1].ramIndex);
        }

        //sort & unique
        std::sort(entriesWithSameRAMIndex.begin(), entriesWithSameRAMIndex.end(), CompareGridEdgeDataByFileIndex());
        std::vector<GridEdgeData>::iterator uniqueEnd = std::unique(entriesWithSameRAMIndex.begin(), entriesWithSameRAMIndex.end());

        //traverse each file bucket and write its contents to disk
        std::vector<GridEdgeData> entriesWithSameFileIndex;
        unsigned fileIndex = entriesWithSameRAMIndex.begin()->fileIndex;

        for(std::vector<GridEdgeData>::iterator it = entriesWithSameRAMIndex.begin(); it != uniqueEnd; it++)
        {
            assert(cellMap->find(it->fileIndex) != cellMap->end() ); //asserting that file index belongs to cell index
            if(it->fileIndex != fileIndex)
            {
                // start in cellIndex vermerken
                int localFileIndex = entriesWithSameFileIndex.begin()->fileIndex;
                int localCellIndex = cellMap->find(localFileIndex)->second;
                int localRamIndex = getRAMIndexFromFileIndex(localFileIndex);
                assert(cellMap->find(entriesWithSameFileIndex.begin()->fileIndex) != cellMap->end());

                cellIndex[localCellIndex] = indexIntoTmpBuffer + fileOffset;
                indexIntoTmpBuffer += FlushEntriesWithSameFileIndexToBuffer(entriesWithSameFileIndex, tmpBuffer, indexIntoTmpBuffer);
                entriesWithSameFileIndex.clear(); //todo: in flushEntries erledigen.
            }
            GridEdgeData data = *it;
            entriesWithSameFileIndex.push_back(data);
            fileIndex = it->fileIndex;
        }
        assert(cellMap->find(entriesWithSameFileIndex.begin()->fileIndex) != cellMap->end());
        int localFileIndex = entriesWithSameFileIndex.begin()->fileIndex;
        int localCellIndex = cellMap->find(localFileIndex)->second;
        int localRamIndex = getRAMIndexFromFileIndex(localFileIndex);

        cellIndex[localCellIndex] = indexIntoTmpBuffer + fileOffset;
        indexIntoTmpBuffer += FlushEntriesWithSameFileIndexToBuffer(entriesWithSameFileIndex, tmpBuffer, indexIntoTmpBuffer);
        entriesWithSameFileIndex.clear(); //todo: in flushEntries erledigen.

        assert(entriesWithSameFileIndex.size() == 0);

        for(int i = 0; i < 32*32; i++)
        {
            indexOutFile.write((char *)&cellIndex[i], sizeof(unsigned));
            numberOfWrittenBytes += sizeof(unsigned);
        }

        //write contents of tmpbuffer to disk
        for(int i = 0; i < indexIntoTmpBuffer; i++)
        {
            indexOutFile.write(&(tmpBuffer->at(i)), sizeof(char));
            numberOfWrittenBytes += sizeof(char);
        }

        delete tmpBuffer;
        delete cellMap;
        return numberOfWrittenBytes;
    }

    unsigned FlushEntriesWithSameFileIndexToBuffer(const std::vector<GridEdgeData> &vectorWithSameFileIndex, vector<char> * tmpBuffer, const unsigned index)
    {
        tmpBuffer->resize(tmpBuffer->size()+(sizeof(NodeID)+sizeof(NodeID)+4*sizeof(int)+sizeof(unsigned))*vectorWithSameFileIndex.size() );
        unsigned counter = 0;
        unsigned max = UINT_MAX;

        for(int i = 0; i < vectorWithSameFileIndex.size()-1; i++)
        {
            assert( vectorWithSameFileIndex[i].fileIndex == vectorWithSameFileIndex[i+1].fileIndex );
            assert( vectorWithSameFileIndex[i].ramIndex == vectorWithSameFileIndex[i+1].ramIndex );
        }

        for(std::vector<GridEdgeData>::const_iterator et = vectorWithSameFileIndex.begin(); et != vectorWithSameFileIndex.end(); et++)
        {
            char * start = (char *)&et->edge.start;
            for(int i = 0; i < sizeof(NodeID); i++)
            {
                tmpBuffer->at(index+counter) = start[i];
                counter++;
            }
            char * target = (char *)&et->edge.target;
            for(int i = 0; i < sizeof(NodeID); i++)
            {
                tmpBuffer->at(index+counter) = target[i];
                counter++;
            }
            char * slat = (char *) &(et->edge.startCoord.lat);
            for(int i = 0; i < sizeof(int); i++)
            {
                tmpBuffer->at(index+counter) = slat[i];
                counter++;
            }
            char * slon = (char *) &(et->edge.startCoord.lon);
            for(int i = 0; i < sizeof(int); i++)
            {
                tmpBuffer->at(index+counter) = slon[i];
                counter++;
            }
            char * tlat = (char *) &(et->edge.targetCoord.lat);
            for(int i = 0; i < sizeof(int); i++)
            {
                tmpBuffer->at(index+counter) = tlat[i];
                counter++;
            }
            char * tlon = (char *) &(et->edge.targetCoord.lon);
            for(int i = 0; i < sizeof(int); i++)
            {
                tmpBuffer->at(index+counter) = tlon[i];
                counter++;
            }
        }
        char * umax = (char *) &max;
        for(int i = 0; i < sizeof(unsigned); i++)
        {
            tmpBuffer->at(index+counter) = umax[i];
            counter++;
        }
        return counter;
    }

    void GetContentsOfFileBucket(const unsigned fileIndex, std::vector<_Edge>& result)
    {
        unsigned ramIndex = getRAMIndexFromFileIndex(fileIndex);
        unsigned startIndexInFile = ramIndexTable[ramIndex];
        if(startIndexInFile == UINT_MAX){
            return;
        }

        std::vector<unsigned> cellIndex;
        cellIndex.resize(32*32);
        google::dense_hash_map< unsigned, unsigned > * cellMap = new google::dense_hash_map< unsigned, unsigned >(1024);
        cellMap->set_empty_key(UINT_MAX);

        indexInFile.seekg(startIndexInFile);

        unsigned lineBase = ramIndex/1024;
        lineBase = lineBase*32*32768;
        unsigned columnBase = ramIndex%1024;
        columnBase=columnBase*32;

        for(int i = 0; i < 32; i++)
        {
            for(int j = 0; j < 32; j++)
            {
                unsigned fileIndex = lineBase + i*32768 + columnBase+j;
                unsigned cellIndex = i*32+j;
                cellMap->insert(std::make_pair(fileIndex, cellIndex));
            }
        }

        unsigned numOfElementsInCell = 0;
        for(int i = 0; i < 32*32; i++)
        {
            indexInFile.read((char *)&cellIndex[i], sizeof(unsigned));
            numOfElementsInCell += cellIndex[i];
        }
        assert(cellMap->find(fileIndex) != cellMap->end());
        if(cellIndex[cellMap->find(fileIndex)->second] == UINT_MAX)
        {
            delete cellMap;
            return;
        }
        unsigned position = cellIndex[cellMap->find(fileIndex)->second] + 32*32*sizeof(unsigned) ;
        indexInFile.seekg(position);
        unsigned numberOfEdgesInFileBucket = 0;
        NodeID start, target; int slat, slon, tlat, tlon;
        do{
            indexInFile.read((char *)&(start), sizeof(NodeID));
            if(start == UINT_MAX || indexInFile.eof())
                break;
            indexInFile.read((char *)&(target), sizeof(NodeID));
            indexInFile.read((char *)&(slat), sizeof(int));
            indexInFile.read((char *)&(slon), sizeof(int));
            indexInFile.read((char *)&(tlat), sizeof(int));
            indexInFile.read((char *)&(tlon), sizeof(int));

            _Edge e(start, target);
            e.startCoord.lat = slat;
            e.startCoord.lon = slon;
            e.targetCoord.lat = tlat;
            e.targetCoord.lon = tlon;

            result.push_back(e);
            numberOfEdgesInFileBucket++;
        } while(true);
        delete cellMap;
    }

    /* More or less from monav project, thanks */
    double ComputeDistance(const _Coordinate& inputPoint, const _Coordinate& source, const _Coordinate& target, _Coordinate& nearest, double *r)
    {
        const double vY = (double)target.lon - (double)source.lon;
        const double vX = (double)target.lat - (double)source.lat;

        const double wY = (double)inputPoint.lon - (double)source.lon;
        const double wX = (double)inputPoint.lat - (double)source.lat;

        const double lengthSquared = vX * vX + vY * vY;

        if(lengthSquared != 0)
        {
            *r = (vX * wX + vY * wY) / lengthSquared;
        }
        double percentage = *r;
        if(*r <=0 ){
            nearest.lat = source.lat;
            nearest.lon = source.lon;
            percentage = 0;
            return wY * wY + wX * wX;
        }
        if( *r>= 1){
            nearest.lat = target.lat;
            nearest.lon = target.lon;
            percentage = 1;
            const double dY = (double)inputPoint.lon - (double)target.lon;
            const double dX = (double)inputPoint.lat - (double)target.lat;
            return dY * dY + dX * dX;
        }

        nearest.lat = (double)source.lat + ( (*r) * vX );
        nearest.lon = (double)source.lon + ( (*r) * vY );
        const double dX = (double)source.lat + (*r) * vX - (double)inputPoint.lat;
        const double dY = (double)source.lon + (*r) * vY - (double)inputPoint.lon;
        return dY*dY + dX*dX;
    }

    ofstream indexOutFile;
    ifstream indexInFile;
    ifstream ramInFile;
    stxxl::vector<GridEdgeData> * entries;
    std::vector<unsigned> ramIndexTable; //4 MB for first level index in RAM
};
}

#endif /* NNGRID_H_ */