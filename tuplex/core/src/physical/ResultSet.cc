//--------------------------------------------------------------------------------------------------------------------//
//                                                                                                                    //
//                                      Tuplex: Blazing Fast Python Data Science                                      //
//                                                                                                                    //
//                                                                                                                    //
//  (c) 2017 - 2021, Tuplex team                                                                                      //
//  Created by Leonhard Spiegelberg first on 1/1/2021                                                                 //
//  License: Apache 2.0                                                                                               //
//--------------------------------------------------------------------------------------------------------------------//

#include <physical/ResultSet.h>
#include <limits>

namespace tuplex {
    ResultSet::ResultSet(const Schema& schema,
            const std::vector<Partition*>& partitions,
            const std::vector<Partition*>& exceptions,
            const std::vector<std::tuple<size_t, PyObject*>> pyobjects,
            int64_t maxRows) : ResultSet::ResultSet() {
        for(Partition *p : partitions)
            _partitions.push_back(p);

        _pyobjects = std::deque<std::tuple<size_t, PyObject*>>(pyobjects.begin(), pyobjects.end());
        _exceptions = exceptions;
        _curRowCounter = 0;
        _totalRowCounter = 0;
        _byteCounter = 0;
        _schema = schema;
        _maxRows = maxRows < 0 ? std::numeric_limits<int64_t>::max() : maxRows;
        _rowsRetrieved = 0;
    }

    void ResultSet::clear() {
        for(auto partition : _partitions)
            partition->invalidate();
        _partitions.clear();
        for(auto partition : _exceptions)
            partition->invalidate();

        _curRowCounter = 0;
        _byteCounter = 0;
        _maxRows = 0;
        _rowsRetrieved = 0;
    }

    bool ResultSet::hasNextRow() {

        // all rows already retrieved?
        if(_rowsRetrieved >= _maxRows)
            return false;

        // empty?
        if(_partitions.empty() && _pyobjects.empty())
            return false;
        else {
            // partitions empty?
            if(_partitions.empty())
                return true;
            else if(_pyobjects.empty()) {
                assert(_partitions.size() > 0);
                assert(_partitions.front());

                // still one row left?
                return _curRowCounter < _partitions.front()->getNumRows();
            } else {
                return true; // there's for sure at least one object left!
            }
        }

    }


    bool ResultSet::hasNextPartition() const {
        // all rows already retrieved?
        if(_rowsRetrieved >= _maxRows)
            return false;

        // empty?
        if(_partitions.empty())
            return false;
        else {
            assert(_partitions.size() > 0);
            assert(_partitions.front());

            // still one row left?
            return _curRowCounter < _partitions.front()->getNumRows();
        }
    }

    Partition* ResultSet::getNextPartition() {
        if(_partitions.empty())
            return nullptr;

        assert(_partitions.size() > 0);

        Partition *first = _partitions.front();
        assert(_schema == first->schema());

        auto numRows = first->getNumRows();
        _rowsRetrieved += numRows;

        _partitions.pop_front();
        _curRowCounter = 0;
        _byteCounter = 0;

        return first;
    }

    Row ResultSet::getNextRow() {
        // merge rows from objects
        if(!_pyobjects.empty()) {
            auto row_number = std::get<0>(_pyobjects.front());
            auto obj = std::get<1>(_pyobjects.front());

            // partitions empty?
            // => simply return next row. no fancy merging possible
            // else merge based on row number.
            if(_partitions.empty() || row_number <= _totalRowCounter) {
                // merge
                python::lockGIL();
                auto row = python::pythonToRow(obj);
                python::unlockGIL();
                _pyobjects.pop_front();
                _rowsRetrieved++;

                // update row counter (not for double indices which could occur from flatMap!)
                if(_pyobjects.empty())
                    _totalRowCounter++;
                else {
                    auto next_row_number = std::get<0>(_pyobjects.front());
                    if(next_row_number != row_number)
                        _totalRowCounter++;
                }

                return row;
            }
        }

        // check whether entry is available, else return empty row
        if(_partitions.empty())
            return Row();

        assert(_partitions.size() > 0);
        Partition *first = _partitions.front();

        // make sure partition schema matches stored schema
        assert(_schema == first->schema());

        Row row;

        // thread safe version (slow)
        // get next element of partition
        const uint8_t* ptr = first->lock();

        row = Row::fromMemory(_schema, ptr + _byteCounter, first->capacity() - _byteCounter);

        // thread safe version (slow)
        // deserialize
        first->unlock();

        _byteCounter += row.serializedLength();
        _curRowCounter++;
        _rowsRetrieved++;
        _totalRowCounter++;

        // get next Partition ready when current one is exhausted
        if(_curRowCounter == first->getNumRows())
            removeFirstPartition();

        return row;
    }

    size_t ResultSet::rowCount() const {
        size_t count = 0;
        for(const auto& partition : _partitions) {
            count += partition->getNumRows();
        }
        return count + _pyobjects.size();
    }

    void ResultSet::removeFirstPartition() {
        assert(_partitions.size() > 0);
        Partition *first = _partitions.front();
        assert(first);

        // invalidate partition
#ifndef NDEBUG
        Logger::instance().defaultLogger().info("ResultSet invalidates partition " + hexAddr(first) + " uuid " + uuidToString(first->uuid()));
#endif
        first->invalidate();

        // remove partition (is now processed)
        _partitions.pop_front();
        _curRowCounter = 0;
        _byteCounter = 0;
    }
}