/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "db/commands/pipeline.h"

#include "db/cursor.h"
#include "db/pipeline/accumulator.h"
#include "db/pipeline/document.h"
#include "db/pipeline/document_source.h"
#include "db/pipeline/expression.h"
#include "db/pdfile.h"

namespace mongo {

    Pipeline::~Pipeline() {
    }

    Pipeline::Pipeline():
	collectionName(),
	sourceList() {
    }

    shared_ptr<Pipeline> Pipeline::parseCommand(
	string &errmsg, BSONObj &cmdObj) {
	shared_ptr<Pipeline> pPipeline(new Pipeline());
        vector<BSONElement> pipeline;

        /* gather the specification for the aggregation */
        for(BSONObj::iterator cmdIterator = cmdObj.begin();
                cmdIterator.more(); ) {
            BSONElement cmdElement(cmdIterator.next());
            const char *pFieldName = cmdElement.fieldName();

            /* look for the aggregation command */
            if (!strcmp(pFieldName, "pipeline")) {
                pipeline = cmdElement.Array();
                continue;
            }

            /* check for the collection name */
            if (!strcmp(pFieldName, "collection")) {
                pPipeline->collectionName = cmdElement.String();
                continue;
            }

            /* we didn't recognize a field in the command */
            ostringstream sb;
            sb <<
               "Pipeline::parseCommand(): unrecognized field \"" <<
               cmdElement.fieldName();
            errmsg = sb.str();
	    return shared_ptr<Pipeline>();
        }

        /*
          If we get here, we've harvested the fields we expect for a pipeline.

          Set up the specified document source pipeline.
        */
	SourceList *pSourceList = &pPipeline->sourceList; // set up shorthand

        /* iterate over the steps in the pipeline */
        const size_t nSteps = pipeline.size();
        for(size_t iStep = 0; iStep < nSteps; ++iStep) {
            /* pull out the pipeline element as an object */
            BSONElement pipeElement(pipeline[iStep]);
            assert(pipeElement.type() == Object); // CW TODO user error
            BSONObj bsonObj(pipeElement.Obj());

	    shared_ptr<DocumentSource> pSource;

            /* use the object to add a DocumentSource to the processing chain */
            BSONObjIterator bsonIterator(bsonObj);
            while(bsonIterator.more()) {
                BSONElement bsonElement(bsonIterator.next());
                const char *pFieldName = bsonElement.fieldName();

                /* select the appropriate operation */
                if (strcmp(pFieldName, "$project") == 0) {
                    pSource =
			DocumentSourceProject::createFromBson(&bsonElement);
		}
                else if (strcmp(pFieldName, "$filter") == 0) {
                    pSource = 
			DocumentSourceFilter::createFromBson(&bsonElement);
		}
                else if (strcmp(pFieldName, "$group") == 0) {
                    pSource = DocumentSourceGroup::createFromBson(&bsonElement);
		}
                else {
                    ostringstream sb;
                    sb <<
                       "Pipeline::run(): unrecognized pipeline op \"" <<
                       pFieldName;
                    errmsg = sb.str();
		    return shared_ptr<Pipeline>();
                }
            }

	    pSourceList->push_back(pSource);
        }

	/*
	  CW TODO - move filters up where possible
	*/

	/* optimize the elements in the pipeline */
	for(SourceList::iterator iter(pSourceList->begin()),
		listEnd(pSourceList->end()); iter != listEnd; ++iter)
	    (*iter)->optimize();

	/*
	  Break down any filters before groups into chunks we can convert for
	  matcher use.

	  LATER -- we've move these further up the chain past projections
	  where possible, remapping field names as they move past projections
	  that rename the fields.
	*/
	// CW TODO

	return pPipeline;
    }

    shared_ptr<Pipeline> Pipeline::splitForSharded() {
	/* create an initialize the shard spec we'll return */
	shared_ptr<Pipeline> pShardPipeline(new Pipeline());
	pShardPipeline->collectionName = collectionName;

	/* put the source list aside */
	SourceList tempList;
	tempList.splice(tempList.begin(), sourceList);

	/*
	  Run through the operations, putting them onto the shard pipeline
	  until we get to a group that indicates a split point.
	 */
	/* look for a grouping operation */
	SourceList::iterator iter(tempList.begin());
	SourceList::iterator listEnd(tempList.end());
	for(; iter != listEnd; ++iter) {
	    shared_ptr<DocumentSource> pSource(*iter);

	    /* copy the operation to the shard pipeline */
	    pShardPipeline->sourceList.push_back(pSource);

	    /* remove it from the temporary list */
	    iter = tempList.erase(iter);

	    DocumentSourceGroup *pGroup =
		dynamic_cast<DocumentSourceGroup *>(pSource.get());
	    if (pGroup) {
		sourceList.push_back(pGroup->createMerger());
		break;
	    }
	}

	return pShardPipeline;
    }

    void Pipeline::toBson(BSONObjBuilder *pBuilder) const {
	assert(false && "unimplemented");
    }

    bool Pipeline::run(BSONObjBuilder &result, string &errmsg,
		       shared_ptr<DocumentSource> pSource) {
	/* chain together the sources we found */
	for(SourceList::iterator iter(sourceList.begin()),
		listEnd(sourceList.end()); iter != listEnd; ++iter) {
	    shared_ptr<DocumentSource> pTemp(*iter);
	    pTemp->setSource(pSource);
	    pSource = pTemp;
	}
	/* pSource is left pointing at the last source in the chain */

        /*
          Iterate through the resulting documents, and add them to the result.
        */
        BSONArrayBuilder resultArray; // where we'll stash the results
        for(bool hasDocument = !pSource->eof(); hasDocument;
                hasDocument = pSource->advance()) {
            shared_ptr<Document> pDocument(pSource->getCurrent());

            /* add the document to the result set */
            BSONObjBuilder documentBuilder;
            pDocument->toBson(&documentBuilder);
            resultArray.append(documentBuilder.done());
        }

        result.appendArray("result", resultArray.done());

        return true;
    }

} // namespace mongo
