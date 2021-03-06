/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/planner_access.h"

#include <algorithm>
#include <vector>

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"

namespace {

    using namespace mongo;

    /**
     * Text node functors.
     */
    bool isTextNode(const QuerySolutionNode* node) {
        return STAGE_TEXT == node->getType();
    }

} // namespace

namespace mongo {

    using std::vector;

    // static
    QuerySolutionNode* QueryPlannerAccess::makeCollectionScan(const CanonicalQuery& query,
                                                              bool tailable,
                                                              const QueryPlannerParams& params) {
        // Make the (only) node, a collection scan.
        CollectionScanNode* csn = new CollectionScanNode();
        csn->name = query.ns();
        csn->filter.reset(query.root()->shallowClone());
        csn->tailable = tailable;
        csn->maxScan = query.getParsed().getMaxScan();

        // If the sort is {$natural: +-1} this changes the direction of the collection scan.
        const BSONObj& sortObj = query.getParsed().getSort();
        if (!sortObj.isEmpty()) {
            BSONElement natural = sortObj.getFieldDotted("$natural");
            if (!natural.eoo()) {
                csn->direction = natural.numberInt() >= 0 ? 1 : -1;
            }
        }

        // The hint can specify $natural as well.
        if (!query.getParsed().getHint().isEmpty()) {
            BSONElement natural = query.getParsed().getHint().getFieldDotted("$natural");
            if (!natural.eoo()) {
                csn->direction = natural.numberInt() >= 0 ? 1 : -1;
            }
        }

        // QLOG() << "Outputting collscan " << soln->toString() << endl;
        return csn;
    }

    // static
    QuerySolutionNode* QueryPlannerAccess::makeLeafNode(const CanonicalQuery& query,
                                                        const IndexEntry& index,
                                                        size_t pos,
                                                        MatchExpression* expr,
                                                        IndexBoundsBuilder::BoundsTightness* tightnessOut) {
        // QLOG() << "making leaf node for " << expr->toString() << endl;
        // We're guaranteed that all GEO_NEARs are first.  This slightly violates the "sort index
        // predicates by their position in the compound index" rule but GEO_NEAR isn't an ixscan.
        // This saves our bacon when we have {foo: 1, bar: "2dsphere"} and the predicate on bar is a
        // $near.  If we didn't get the GEO_NEAR first we'd create an IndexScanNode and later cast
        // it to a GeoNear2DSphereNode
        //
        // This should gracefully deal with the case where we have a pred over foo but no geo clause
        // over bar.  In that case there is no GEO_NEAR to appear first and it's treated like a
        // straight ixscan.
        BSONElement elt = index.keyPattern.firstElement();
        bool indexIs2D = (String == elt.type() && "2d" == elt.String());

        if (MatchExpression::GEO_NEAR == expr->matchType()) {
            // We must not keep the expression node around.
            *tightnessOut = IndexBoundsBuilder::EXACT;
            GeoNearMatchExpression* nearExpr = static_cast<GeoNearMatchExpression*>(expr);
            // 2d geoNear requires a hard limit and as such we take it out before it gets here.  If
            // this happens it's a bug.
            verify(!indexIs2D);
            GeoNear2DSphereNode* ret = new GeoNear2DSphereNode();
            ret->indexKeyPattern = index.keyPattern;
            ret->nq = nearExpr->getData();
            ret->baseBounds.fields.resize(index.keyPattern.nFields());
            if (NULL != query.getProj()) {
                ret->addPointMeta = query.getProj()->wantGeoNearPoint();
                ret->addDistMeta = query.getProj()->wantGeoNearDistance();
            }
            return ret;
        }
        else if (indexIs2D) {
            // We must not keep the expression node around.
            *tightnessOut = IndexBoundsBuilder::EXACT;
            verify(MatchExpression::GEO == expr->matchType());
            GeoMatchExpression* nearExpr = static_cast<GeoMatchExpression*>(expr);
            verify(indexIs2D);
            Geo2DNode* ret = new Geo2DNode();
            ret->indexKeyPattern = index.keyPattern;
            ret->gq = nearExpr->getGeoQuery();
            return ret;
        }
        else if (MatchExpression::TEXT == expr->matchType()) {
            // We must not keep the expression node around.
            *tightnessOut = IndexBoundsBuilder::EXACT;
            TextMatchExpression* textExpr = static_cast<TextMatchExpression*>(expr);
            TextNode* ret = new TextNode();
            ret->indexKeyPattern = index.keyPattern;
            ret->query = textExpr->getQuery();
            ret->language = textExpr->getLanguage();
            return ret;
        }
        else {
            // QLOG() << "making ixscan for " << expr->toString() << endl;

            // Note that indexKeyPattern.firstElement().fieldName() may not equal expr->path()
            // because expr might be inside an array operator that provides a path prefix.
            IndexScanNode* isn = new IndexScanNode();
            isn->indexKeyPattern = index.keyPattern;
            isn->indexIsMultiKey = index.multikey;
            isn->bounds.fields.resize(index.keyPattern.nFields());
            isn->maxScan = query.getParsed().getMaxScan();
            isn->addKeyMetadata = query.getParsed().returnKey();

            // Get the ixtag->pos-th element of the index key pattern.
            // TODO: cache this instead/with ixtag->pos?
            BSONObjIterator it(index.keyPattern);
            BSONElement keyElt = it.next();
            for (size_t i = 0; i < pos; ++i) {
                verify(it.more());
                keyElt = it.next();
            }
            verify(!keyElt.eoo());

            IndexBoundsBuilder::translate(expr, keyElt, index, &isn->bounds.fields[pos],
                                          tightnessOut);

            // QLOG() << "bounds are " << isn->bounds.toString() << " exact " << *exact << endl;
            return isn;
        }
    }

    bool QueryPlannerAccess::shouldMergeWithLeaf(const MatchExpression* expr,
                                                 const IndexEntry& index,
                                                 size_t pos,
                                                 QuerySolutionNode* node,
                                                 MatchExpression::MatchType mergeType) {
        if (NULL == node || NULL == expr) {
            return false;
        }

        const StageType type = node->getType();
        verify(STAGE_GEO_NEAR_2D != type);

        if (STAGE_GEO_2D == type || STAGE_TEXT == type ||
            STAGE_GEO_NEAR_2DSPHERE == type) {
            return true;
        }

        invariant(type == STAGE_IXSCAN);
        IndexScanNode* scan = static_cast<IndexScanNode*>(node);
        IndexBounds* boundsToFillOut =  &scan->bounds;

        if (boundsToFillOut->fields[pos].name.empty()) {
            // The bounds will be compounded. This is OK because the
            // plan enumerator told us that it is OK.
            return true;
        }
        else {
            if (MatchExpression::AND == mergeType) {
                // The bounds will be intersected. This is OK provided
                // that the index is NOT multikey.
                return !index.multikey;
            }
            else {
                // The bounds will be unionized.
                return true;
            }
        }


    }

    void QueryPlannerAccess::mergeWithLeafNode(MatchExpression* expr,
                                               const IndexEntry& index,
                                               size_t pos,
                                               IndexBoundsBuilder::BoundsTightness* tightnessOut,
                                               QuerySolutionNode* node,
                                               MatchExpression::MatchType mergeType) {

        const StageType type = node->getType();
        verify(STAGE_GEO_NEAR_2D != type);

        if (STAGE_GEO_2D == type) {
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
            return;
        }

        // Text data is covered, but not exactly.  Text covering is unlike any other covering
        // so we deal with it in _addFilterToSolutionNode.
        if (STAGE_TEXT == type) {
            *tightnessOut = IndexBoundsBuilder::INEXACT_COVERED;
            return;
        }

        IndexBounds* boundsToFillOut = NULL;

        if (STAGE_GEO_NEAR_2DSPHERE == type) {
            GeoNear2DSphereNode* gn = static_cast<GeoNear2DSphereNode*>(node);
            boundsToFillOut = &gn->baseBounds;
        }
        else {
            verify(type == STAGE_IXSCAN);
            IndexScanNode* scan = static_cast<IndexScanNode*>(node);
            boundsToFillOut = &scan->bounds;
        }

        // Get the ixtag->pos-th element of the index key pattern.
        // TODO: cache this instead/with ixtag->pos?
        BSONObjIterator it(index.keyPattern);
        BSONElement keyElt = it.next();
        for (size_t i = 0; i < pos; ++i) {
            verify(it.more());
            keyElt = it.next();
        }
        verify(!keyElt.eoo());
        *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;

        verify(boundsToFillOut->fields.size() > pos);

        OrderedIntervalList* oil = &boundsToFillOut->fields[pos];

        if (boundsToFillOut->fields[pos].name.empty()) {
            IndexBoundsBuilder::translate(expr, keyElt, index, oil, tightnessOut);
        }
        else {
            if (MatchExpression::AND == mergeType) {
                IndexBoundsBuilder::translateAndIntersect(expr, keyElt, index, oil, tightnessOut);
            }
            else {
                verify(MatchExpression::OR == mergeType);
                IndexBoundsBuilder::translateAndUnion(expr, keyElt, index, oil, tightnessOut);
            }
        }
    }

    // static
    void QueryPlannerAccess::finishTextNode(QuerySolutionNode* node, const IndexEntry& index) {
        TextNode* tn = static_cast<TextNode*>(node);

        // Figure out what positions are prefix positions.  We build an index key prefix from
        // the predicates over the text index prefix keys.
        // For example, say keyPattern = { a: 1, _fts: "text", _ftsx: 1, b: 1 }
        // prefixEnd should be 1.
        size_t prefixEnd = 0;
        BSONObjIterator it(tn->indexKeyPattern);
        // Count how many prefix terms we have.
        while (it.more()) {
            // We know that the only key pattern with a type of String is the _fts field
            // which is immediately after all prefix fields.
            if (String == it.next().type()) {
                break;
            }
            ++prefixEnd;
        }

        // If there's no prefix, the filter is already on the node and the index prefix is null.
        // We can just return.
        if (!prefixEnd) {
            return;
        }

        // We can't create a text stage if there aren't EQ predicates on its prefix terms.  So
        // if we've made it this far, we should have collected the prefix predicates in the
        // filter.
        invariant(NULL != tn->filter.get());
        MatchExpression* textFilterMe = tn->filter.get();

        BSONObjBuilder prefixBob;

        if (MatchExpression::AND != textFilterMe->matchType()) {
            // Only one prefix term.
            invariant(1 == prefixEnd);
            // Sanity check: must be an EQ.
            invariant(MatchExpression::EQ == textFilterMe->matchType());

            EqualityMatchExpression* eqExpr = static_cast<EqualityMatchExpression*>(textFilterMe);
            prefixBob.append(eqExpr->getData());
            tn->filter.reset();
        }
        else {
            invariant(MatchExpression::AND == textFilterMe->matchType());

            // Indexed by the keyPattern position index assignment.  We want to add
            // prefixes in order but we must order them first.
            vector<MatchExpression*> prefixExprs(prefixEnd, NULL);

            AndMatchExpression* amExpr = static_cast<AndMatchExpression*>(textFilterMe);
            invariant(amExpr->numChildren() >= prefixEnd);

            // Look through the AND children.  The prefix children we want to
            // stash in prefixExprs.
            size_t curChild = 0;
            while (curChild < amExpr->numChildren()) {
                MatchExpression* child = amExpr->getChild(curChild);
                IndexTag* ixtag = static_cast<IndexTag*>(child->getTag());
                invariant(NULL != ixtag);
                // Only want prefixes.
                if (ixtag->pos >= prefixEnd) {
                    ++curChild;
                    continue;
                }
                prefixExprs[ixtag->pos] = child;
                amExpr->getChildVector()->erase(amExpr->getChildVector()->begin() + curChild);
                // Don't increment curChild.
            }

            // Go through the prefix equalities in order and create an index prefix out of them.
            for (size_t i = 0; i < prefixExprs.size(); ++i) {
                MatchExpression* prefixMe = prefixExprs[i];
                invariant(NULL != prefixMe);
                invariant(MatchExpression::EQ == prefixMe->matchType());
                EqualityMatchExpression* eqExpr = static_cast<EqualityMatchExpression*>(prefixMe);
                prefixBob.append(eqExpr->getData());
                // We removed this from the AND expression that owned it, so we must clean it
                // up ourselves.
                delete prefixMe;
            }

            // Clear out an empty $and.
            if (0 == amExpr->numChildren()) {
                tn->filter.reset();
            }
            else if (1 == amExpr->numChildren()) {
                // Clear out unsightly only child of $and
                MatchExpression* child = amExpr->getChild(0);
                amExpr->getChildVector()->clear();
                // Deletes current filter which is amExpr.
                tn->filter.reset(child);
            }
        }

        tn->indexPrefix = prefixBob.obj();
    }

    // static
    void QueryPlannerAccess::finishLeafNode(QuerySolutionNode* node, const IndexEntry& index) {
        const StageType type = node->getType();
        verify(STAGE_GEO_NEAR_2D != type);

        if (STAGE_GEO_2D == type) {
            return;
        }

        if (STAGE_TEXT == type) {
            finishTextNode(node, index);
            return;
        }

        IndexBounds* bounds = NULL;

        if (STAGE_GEO_NEAR_2DSPHERE == type) {
            GeoNear2DSphereNode* gnode = static_cast<GeoNear2DSphereNode*>(node);
            bounds = &gnode->baseBounds;
        }
        else {
            verify(type == STAGE_IXSCAN);
            IndexScanNode* scan = static_cast<IndexScanNode*>(node);
            bounds = &scan->bounds;
        }

        // XXX: this currently fills out minkey/maxkey bounds for near queries, fix that.  just
        // set the field name of the near query field when starting a near scan.

        // Find the first field in the scan's bounds that was not filled out.
        // TODO: could cache this.
        size_t firstEmptyField = 0;
        for (firstEmptyField = 0; firstEmptyField < bounds->fields.size(); ++firstEmptyField) {
            if ("" == bounds->fields[firstEmptyField].name) {
                verify(bounds->fields[firstEmptyField].intervals.empty());
                break;
            }
        }

        // All fields are filled out with bounds, nothing to do.
        if (firstEmptyField == bounds->fields.size()) {
            IndexBoundsBuilder::alignBounds(bounds, index.keyPattern);
            return;
        }

        // Skip ahead to the firstEmptyField-th element, where we begin filling in bounds.
        BSONObjIterator it(index.keyPattern);
        for (size_t i = 0; i < firstEmptyField; ++i) {
            verify(it.more());
            it.next();
        }

        // For each field in the key...
        while (it.more()) {
            BSONElement kpElt = it.next();
            // There may be filled-in fields to the right of the firstEmptyField.
            // Example:
            // The index {loc:"2dsphere", x:1}
            // With a predicate over x and a near search over loc.
            if ("" == bounds->fields[firstEmptyField].name) {
                verify(bounds->fields[firstEmptyField].intervals.empty());
                // ...build the "all values" interval.
                IndexBoundsBuilder::allValuesForField(kpElt,
                                                      &bounds->fields[firstEmptyField]);
            }
            ++firstEmptyField;
        }

        // Make sure that the length of the key is the length of the bounds we started.
        verify(firstEmptyField == bounds->fields.size());

        // We create bounds assuming a forward direction but can easily reverse bounds to align
        // according to our desired direction.
        IndexBoundsBuilder::alignBounds(bounds, index.keyPattern);
    }

    // static
    void QueryPlannerAccess::findElemMatchChildren(const MatchExpression* node,
                                                   vector<MatchExpression*>* out) {
        for (size_t i = 0; i < node->numChildren(); ++i) {
            MatchExpression* child = node->getChild(i);
            if (Indexability::nodeCanUseIndexOnOwnField(child) &&
                NULL != child->getTag()) {
                out->push_back(child);
            }
            else if (MatchExpression::AND == child->matchType() ||
                     MatchExpression::ELEM_MATCH_OBJECT == child->matchType()) {
                findElemMatchChildren(child, out);
            }
        }
    }

    // static
    bool QueryPlannerAccess::processIndexScans(const CanonicalQuery& query,
                                         MatchExpression* root,
                                         bool inArrayOperator,
                                         const vector<IndexEntry>& indices,
                                         vector<QuerySolutionNode*>* out) {

        auto_ptr<QuerySolutionNode> currentScan;
        size_t currentIndexNumber = IndexTag::kNoIndex;
        size_t curChild = 0;

        // This 'while' processes all IXSCANs, possibly merging scans by combining the bounds.  We
        // can merge scans in two cases:
        // 1. Filling out subsequent fields in a compound index.
        // 2. Intersecting bounds.  Currently unimplemented.
        while (curChild < root->numChildren()) {
            MatchExpression* child = root->getChild(curChild);

            // If there is no tag, it's not using an index.  We've sorted our children such that the
            // children with tags are first, so we stop now.
            if (NULL == child->getTag()) { break; }

            IndexTag* ixtag = static_cast<IndexTag*>(child->getTag());
            // If there's a tag it must be valid.
            verify(IndexTag::kNoIndex != ixtag->index);

            // If the child can't use an index on its own field (and the child is not a negation
            // of a bounds-generating expression), then it's indexed by virtue of one of
            // its children having an index.
            //
            // If the child is an $elemMatch, we try to merge its child predicates into the
            // current ixscan.
            //
            // NOTE: If the child is logical, it could possibly collapse into a single ixscan.  we
            // ignore this for now.
            if (!Indexability::isBoundsGenerating(child)) {
                // If we're here, then the child is indexed by virtue of its children.
                // In most cases this means that we recursively build indexed data
                // access on 'child'.

                if (MatchExpression::AND == root->matchType() &&
                    MatchExpression::ELEM_MATCH_OBJECT == child->matchType()) {
                    // We have an AND with an ELEM_MATCH_OBJECT child. The plan enumerator produces
                    // index taggings which indicate that we should try to compound with
                    // predicates retrieved from inside the subtree rooted at the ELEM_MATCH.
                    // In order to obey the enumerator's tagging, we need to retrieve these
                    // predicates from inside the $elemMatch, and try to merge them with
                    // the current index scan.

                    // Populate 'emChildren' with tagged predicates from inside the
                    // tree rooted at 'child.
                    vector<MatchExpression*> emChildren;
                    findElemMatchChildren(child, &emChildren);

                    // For each predicate in 'emChildren', try to merge it with the
                    // current index scan.
                    //
                    // This loop is identical to the outer loop except for two
                    // changes:
                    //  1) The OR case is removed. We would never hit the OR case
                    //  because we've already checked that the matchType of 'root'
                    //  is AND.
                    //  2) We want to leave the entire $elemMatch in place as a
                    //  child of the parent AND. This way, the calling function
                    //  will affix the entire $elemMatch expression as a filter
                    //  above the AND.
                    for (size_t i = 0; i < emChildren.size(); ++i) {
                        MatchExpression* emChild = emChildren[i];
                        invariant(NULL != emChild->getTag());
                        IndexTag* innerTag = static_cast<IndexTag*>(emChild->getTag());

                        if (NULL != currentScan.get() && (currentIndexNumber == ixtag->index) &&
                            shouldMergeWithLeaf(emChild, indices[currentIndexNumber], innerTag->pos,
                                                currentScan.get(), root->matchType())) {
                            // The child uses the same index we're currently building a scan for.  Merge
                            // the bounds and filters.
                            verify(currentIndexNumber == ixtag->index);

                            IndexBoundsBuilder::BoundsTightness tightness = IndexBoundsBuilder::INEXACT_FETCH;
                            mergeWithLeafNode(emChild, indices[currentIndexNumber], innerTag->pos, &tightness,
                                              currentScan.get(), root->matchType());

                            if (tightness == IndexBoundsBuilder::INEXACT_COVERED
                                     && !indices[currentIndexNumber].multikey) {
                                // Add the filter to the current index scan. This is optional because
                                // the entire filter will get affixed to the parent AND. It is here
                                // as an optimization---an additional filter during the index scan
                                // stage will cause fewer documents to bubble up to the parent node
                                // of the execution tree.
                                _addFilterToSolutionNode(currentScan.get(), emChild, root->matchType());
                            }
                        }
                        else {
                            if (NULL != currentScan.get()) {
                                finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
                                out->push_back(currentScan.release());
                            }
                            else {
                                verify(IndexTag::kNoIndex == currentIndexNumber);
                            }

                            currentIndexNumber = ixtag->index;

                            IndexBoundsBuilder::BoundsTightness tightness = IndexBoundsBuilder::INEXACT_FETCH;
                            currentScan.reset(makeLeafNode(query, indices[currentIndexNumber], innerTag->pos,
                                                            emChild, &tightness));

                            if (tightness == IndexBoundsBuilder::INEXACT_COVERED
                                     && !indices[currentIndexNumber].multikey) {
                                // Add the filter to the current index scan. This is optional because
                                // the entire filter will get affixed to the parent AND. It is here
                                // as an optimization---an additional filter during the index scan
                                // stage will cause fewer documents to bubble up to the parent node
                                // of the execution tree.
                                _addFilterToSolutionNode(currentScan.get(), emChild, root->matchType());
                            }
                        }
                    }

                    // We're done processing the $elemMatch child. We leave it hanging off
                    // it's AND parent so that it will be affixed as a filter later on,
                    // and move on to the next child of the AND.
                    ++curChild;
                    continue;
                }
                else if (!inArrayOperator) {
                    // The logical sub-tree is responsible for fully evaluating itself.  Any
                    // required filters or fetches are already hung on it.  As such, we remove the
                    // filter branch from our tree.  buildIndexedDataAccess takes ownership of the
                    // child.
                    root->getChildVector()->erase(root->getChildVector()->begin() + curChild);
                    // The curChild of today is the curChild+1 of yesterday.
                }
                else {
                    ++curChild;
                }

                // If inArrayOperator: takes ownership of child, which is OK, since we detached
                // child from root.
                QuerySolutionNode* childSolution = buildIndexedDataAccess(query,
                                                                          child,
                                                                          inArrayOperator,
                                                                          indices);
                if (NULL == childSolution) { return false; }
                out->push_back(childSolution);
                continue;
            }

            // If we're here, we now know that 'child' can use an index directly and the index is
            // over the child's field.

            // If 'child' is a NOT, then the tag we're interested in is on the NOT's
            // child node.
            if (MatchExpression::NOT == child->matchType()) {
                ixtag = static_cast<IndexTag*>(child->getChild(0)->getTag());
                invariant(IndexTag::kNoIndex != ixtag->index);
            }

            // If the child we're looking at uses a different index than the current index scan, add
            // the current index scan to the output as we're done with it.  The index scan created
            // by the child then becomes our new current index scan.  Note that the current scan
            // could be NULL, in which case we don't output it.  The rest of the logic is identical.
            //
            // If the child uses the same index as the current index scan, we may be able to merge
            // the bounds for the two scans.
            //
            // Guiding principle: must the values we're testing come from the same array in the
            // document?  If so, we can combine bounds (via intersection or compounding).  If not,
            // we can't.
            //
            // If the index is NOT multikey, it's always semantically correct to combine bounds,
            // as there are no arrays to worry about.
            //
            // If the index is multikey, there are arrays of values.  There are several
            // complications in the multikey case that have to be obeyed both by the enumerator
            // and here as we try to merge predicates into query solution leaves. The hairy
            // details of these rules are documented near the top of planner_access.h.
            if (NULL != currentScan.get() && (currentIndexNumber == ixtag->index) &&
                shouldMergeWithLeaf(child, indices[currentIndexNumber], ixtag->pos,
                                    currentScan.get(), root->matchType())) {
                // The child uses the same index we're currently building a scan for.  Merge
                // the bounds and filters.
                verify(currentIndexNumber == ixtag->index);

                IndexBoundsBuilder::BoundsTightness tightness = IndexBoundsBuilder::INEXACT_FETCH;
                mergeWithLeafNode(child, indices[currentIndexNumber], ixtag->pos, &tightness,
                                  currentScan.get(), root->matchType());

                if (tightness == IndexBoundsBuilder::EXACT) {
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);
                    delete child;
                }
                else if (tightness == IndexBoundsBuilder::INEXACT_COVERED
                         && (INDEX_TEXT == indices[currentIndexNumber].type
                             || !indices[currentIndexNumber].multikey)) {
                    // The bounds are not exact, but the information needed to
                    // evaluate the predicate is in the index key. Remove the
                    // MatchExpression from its parent and attach it to the filter
                    // of the index scan we're building.
                    //
                    // We can only use this optimization if the index is NOT multikey.
                    // Suppose that we had the multikey index {x: 1} and a document
                    // {x: ["a", "b"]}. Now if we query for {x: /b/} the filter might
                    // ever only be applied to the index key "a". We'd incorrectly
                    // conclude that the document does not match the query :( so we
                    // gotta stick to non-multikey indices.
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);

                    _addFilterToSolutionNode(currentScan.get(), child, root->matchType());
                }
                else if (root->matchType() == MatchExpression::OR) {
                    // In the AND case, the filter can be brought above the AND node.
                    // But in the OR case, the filter only applies to one branch, so
                    // we must affix curChild's filter now. In order to apply the filter
                    // to the proper OR branch, create a FETCH node with the filter whose
                    // child is the IXSCAN.
                    finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);

                    FetchNode* fetch = new FetchNode();
                    // takes ownership
                    fetch->filter.reset(child);
                    // takes ownership
                    fetch->children.push_back(currentScan.release());
                    // takes ownership
                    out->push_back(fetch);

                    currentIndexNumber = IndexTag::kNoIndex;
                }
                else {
                    // We keep curChild in the AND for affixing later.
                    ++curChild;
                }
            }
            else {
                if (NULL != currentScan.get()) {
                    finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
                    out->push_back(currentScan.release());
                }
                else {
                    verify(IndexTag::kNoIndex == currentIndexNumber);
                }

                currentIndexNumber = ixtag->index;

                IndexBoundsBuilder::BoundsTightness tightness = IndexBoundsBuilder::INEXACT_FETCH;
                currentScan.reset(makeLeafNode(query, indices[currentIndexNumber], ixtag->pos,
                                                child, &tightness));

                if (tightness == IndexBoundsBuilder::EXACT && !inArrayOperator) {
                    // The bounds answer the predicate, and we can remove the expression from the
                    // root.  NOTE(opt): Erasing entry 0, 1, 2, ... could be kind of n^2, maybe
                    // optimize later.
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);
                    delete child;
                    // Don't increment curChild.
                }
                else if (tightness == IndexBoundsBuilder::INEXACT_COVERED
                         && !indices[currentIndexNumber].multikey) {
                    // The bounds are not exact, but the information needed to
                    // evaluate the predicate is in the index key. Remove the
                    // MatchExpression from its parent and attach it to the filter
                    // of the index scan we're building.
                    //
                    // We can only use this optimization if the index is NOT multikey.
                    // Suppose that we had the multikey index {x: 1} and a document
                    // {x: ["a", "b"]}. Now if we query for {x: /b/} the filter might
                    // ever only be applied to the index key "a". We'd incorrectly
                    // conclude that the document does not match the query :( so we
                    // gotta stick to non-multikey indices.
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);

                    _addFilterToSolutionNode(currentScan.get(), child, root->matchType());
                }
                else if (root->matchType() == MatchExpression::OR) {
                    // In the AND case, the filter can be brought above the AND node.
                    // But in the OR case, the filter only applies to one branch, so
                    // we must affix curChild's filter now. In order to apply the filter
                    // to the proper OR branch, create a FETCH node with the filter whose
                    // child is the IXSCAN.
                    finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
                    root->getChildVector()->erase(root->getChildVector()->begin()
                                                  + curChild);

                    FetchNode* fetch = new FetchNode();
                    // takes ownership
                    fetch->filter.reset(child);
                    // takes ownership
                    fetch->children.push_back(currentScan.release());
                    // takes ownership
                    out->push_back(fetch);

                    currentIndexNumber = IndexTag::kNoIndex;
                }
                else {
                    // We keep curChild in the AND for affixing later as a filter.
                    ++curChild;
                }
            }
        }

        // Output the scan we're done with, if it exists.
        if (NULL != currentScan.get()) {
            finishLeafNode(currentScan.get(), indices[currentIndexNumber]);
            out->push_back(currentScan.release());
        }

        return true;
    }

    // static
    QuerySolutionNode* QueryPlannerAccess::buildIndexedAnd(const CanonicalQuery& query,
                                                     MatchExpression* root,
                                                     bool inArrayOperator,
                                                     const vector<IndexEntry>& indices) {
        auto_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        vector<QuerySolutionNode*> ixscanNodes;
        if (!processIndexScans(query, root, inArrayOperator, indices, &ixscanNodes)) {
            return NULL;
        }

        //
        // Process all non-indexed predicates.  We hang these above the AND with a fetch and
        // filter.
        //

        // This is the node we're about to return.
        QuerySolutionNode* andResult;

        // We must use an index for at least one child of the AND.  We shouldn't be here if this
        // isn't the case.
        verify(ixscanNodes.size() >= 1);

        // Short-circuit: an AND of one child is just the child.
        if (ixscanNodes.size() == 1) {
            andResult = ixscanNodes[0];
        }
        else {
            // Figure out if we want AndHashNode or AndSortedNode.
            bool allSortedByDiskLoc = true;
            for (size_t i = 0; i < ixscanNodes.size(); ++i) {
                if (!ixscanNodes[i]->sortedByDiskLoc()) {
                    allSortedByDiskLoc = false;
                    break;
                }
            }
            if (allSortedByDiskLoc) {
                AndSortedNode* asn = new AndSortedNode();
                asn->children.swap(ixscanNodes);
                andResult = asn;
            }
            else {
                AndHashNode* ahn = new AndHashNode();
                ahn->children.swap(ixscanNodes);
                andResult = ahn;
                // The AndHashNode provides the sort order of its last child.  If any of the
                // possible subnodes of AndHashNode provides the sort order we care about, we put
                // that one last.
                for (size_t i = 0; i < ahn->children.size(); ++i) {
                    ahn->children[i]->computeProperties();
                    const BSONObjSet& sorts = ahn->children[i]->getSort();
                    if (sorts.end() != sorts.find(query.getParsed().getSort())) {
                        std::swap(ahn->children[i], ahn->children.back());
                        break;
                    }
                }
            }
        }

        // Don't bother doing any kind of fetch analysis lite if we're doing it anyway above us.
        if (inArrayOperator) {
            return andResult;
        }

        // If there are any nodes still attached to the AND, we can't answer them using the
        // index, so we put a fetch with filter.
        if (root->numChildren() > 0) {
            FetchNode* fetch = new FetchNode();
            verify(NULL != autoRoot.get());
            if (autoRoot->numChildren() == 1) {
                // An $and of one thing is that thing.
                MatchExpression* child = autoRoot->getChild(0);
                autoRoot->getChildVector()->clear();
                // Takes ownership.
                fetch->filter.reset(child);
                // 'autoRoot' will delete the empty $and.
            }
            else { // root->numChildren() > 1
                // Takes ownership.
                fetch->filter.reset(autoRoot.release());
            }
            // takes ownership
            fetch->children.push_back(andResult);
            andResult = fetch;
        }
        else {
            // root has no children, let autoRoot get rid of it when it goes out of scope.
        }

        return andResult;
    }

    // static
    QuerySolutionNode* QueryPlannerAccess::buildIndexedOr(const CanonicalQuery& query,
                                                    MatchExpression* root,
                                                    bool inArrayOperator,
                                                    const vector<IndexEntry>& indices) {
        auto_ptr<MatchExpression> autoRoot;
        if (!inArrayOperator) {
            autoRoot.reset(root);
        }

        vector<QuerySolutionNode*> ixscanNodes;
        if (!processIndexScans(query, root, inArrayOperator, indices, &ixscanNodes)) {
            return NULL;
        }

        // Unlike an AND, an OR cannot have filters hanging off of it.  We stop processing
        // when any of our children lack index tags.  If a node lacks an index tag it cannot
        // be answered via an index.
        if (!inArrayOperator && 0 != root->numChildren()) {
            warning() << "planner OR error, non-indexed child of OR.";
            // We won't enumerate an OR without indices for each child, so this isn't an issue, even
            // if we have an AND with an OR child -- we won't get here unless the OR is fully
            // indexed.
            return NULL;
        }

        QuerySolutionNode* orResult = NULL;

        // An OR of one node is just that node.
        if (1 == ixscanNodes.size()) {
            orResult = ixscanNodes[0];
        }
        else {
            bool shouldMergeSort = false;

            if (!query.getParsed().getSort().isEmpty()) {
                const BSONObj& desiredSort = query.getParsed().getSort();

                // If there exists a sort order that is present in each child, we can merge them and
                // maintain that sort order / those sort orders.
                ixscanNodes[0]->computeProperties();
                BSONObjSet sharedSortOrders = ixscanNodes[0]->getSort();

                if (!sharedSortOrders.empty()) {
                    for (size_t i = 1; i < ixscanNodes.size(); ++i) {
                        ixscanNodes[i]->computeProperties();
                        BSONObjSet isect;
                        set_intersection(sharedSortOrders.begin(),
                                sharedSortOrders.end(),
                                ixscanNodes[i]->getSort().begin(),
                                ixscanNodes[i]->getSort().end(),
                                std::inserter(isect, isect.end()),
                                BSONObjCmp());
                        sharedSortOrders = isect;
                        if (sharedSortOrders.empty()) {
                            break;
                        }
                    }
                }

                // XXX: consider reversing?
                shouldMergeSort = (sharedSortOrders.end() != sharedSortOrders.find(desiredSort));
            }

            if (shouldMergeSort) {
                MergeSortNode* msn = new MergeSortNode();
                msn->sort = query.getParsed().getSort();
                msn->children.swap(ixscanNodes);
                orResult = msn;
            }
            else {
                OrNode* orn = new OrNode();
                orn->children.swap(ixscanNodes);
                orResult = orn;
            }
        }

        // Evaluate text nodes first to ensure that text scores are available.
        // Move text nodes to front of vector.
        std::stable_partition(orResult->children.begin(), orResult->children.end(), isTextNode);

        // OR must have an index for each child, so we should have detached all children from
        // 'root', and there's nothing useful to do with an empty or MatchExpression.  We let it die
        // via autoRoot.

        return orResult;
    }

    // static
    QuerySolutionNode* QueryPlannerAccess::buildIndexedDataAccess(const CanonicalQuery& query,
                                                            MatchExpression* root,
                                                            bool inArrayOperator,
                                                            const vector<IndexEntry>& indices) {
        if (root->isLogical() && !Indexability::isBoundsGeneratingNot(root)) {
            if (MatchExpression::AND == root->matchType()) {
                // Takes ownership of root.
                return buildIndexedAnd(query, root, inArrayOperator, indices);
            }
            else if (MatchExpression::OR == root->matchType()) {
                // Takes ownership of root.
                return buildIndexedOr(query, root, inArrayOperator, indices);
            }
            else {
                // Can't do anything with negated logical nodes index-wise.
                return NULL;
            }
        }
        else {
            auto_ptr<MatchExpression> autoRoot;
            if (!inArrayOperator) {
                autoRoot.reset(root);
            }

            // isArray or isLeaf is true.  Either way, it's over one field, and the bounds builder
            // deals with it.
            if (NULL == root->getTag()) {
                // No index to use here, not in the context of logical operator, so we're SOL.
                return NULL;
            }
            else if (Indexability::isBoundsGenerating(root)) {
                // Make an index scan over the tagged index #.
                IndexTag* tag = static_cast<IndexTag*>(root->getTag());

                IndexBoundsBuilder::BoundsTightness tightness = IndexBoundsBuilder::EXACT;
                QuerySolutionNode* soln = makeLeafNode(query, indices[tag->index], tag->pos,
                                                       root, &tightness);
                verify(NULL != soln);
                finishLeafNode(soln, indices[tag->index]);

                if (inArrayOperator) {
                    return soln;
                }

                // If the bounds are exact, the set of documents that satisfy the predicate is
                // exactly equal to the set of documents that the scan provides.
                //
                // If the bounds are not exact, the set of documents returned from the scan is a
                // superset of documents that satisfy the predicate, and we must check the
                // predicate.

                if (tightness == IndexBoundsBuilder::EXACT) {
                    return soln;
                }
                else if (tightness == IndexBoundsBuilder::INEXACT_COVERED
                         && !indices[tag->index].multikey) {
                    verify(NULL == soln->filter.get());
                    soln->filter.reset(autoRoot.release());
                    return soln;
                }
                else {
                    FetchNode* fetch = new FetchNode();
                    verify(NULL != autoRoot.get());
                    fetch->filter.reset(autoRoot.release());
                    fetch->children.push_back(soln);
                    return fetch;
                }
            }
            else if (Indexability::arrayUsesIndexOnChildren(root)) {
                QuerySolutionNode* solution = NULL;

                if (MatchExpression::ALL == root->matchType()) {
                    // Here, we formulate an AND of all the sub-clauses.
                    auto_ptr<AndHashNode> ahn(new AndHashNode());

                    for (size_t i = 0; i < root->numChildren(); ++i) {
                        QuerySolutionNode* node = buildIndexedDataAccess(query,
                                                                         root->getChild(i),
                                                                         true,
                                                                         indices);
                        if (NULL != node) {
                            ahn->children.push_back(node);
                        }
                    }

                    // No children, no point in hashing nothing.
                    if (0 == ahn->children.size()) { return NULL; }

                    // AND of one child is just that child.
                    if (1 == ahn->children.size()) {
                        solution = ahn->children[0];
                        ahn->children.clear();
                        ahn.reset();
                    }
                    else {
                        // More than one child.
                        solution = ahn.release();
                    }
                }
                else {
                    verify(MatchExpression::ELEM_MATCH_OBJECT);
                    // The child is an AND.
                    verify(1 == root->numChildren());
                    solution = buildIndexedDataAccess(query, root->getChild(0), true, indices);
                    if (NULL == solution) { return NULL; }
                }

                // There may be an array operator above us.
                if (inArrayOperator) { return solution; }

                FetchNode* fetch = new FetchNode();
                // Takes ownership of 'root'.
                verify(NULL != autoRoot.get());
                fetch->filter.reset(autoRoot.release());
                fetch->children.push_back(solution);
                return fetch;
            }
        }

        return NULL;
    }

    QuerySolutionNode* QueryPlannerAccess::scanWholeIndex(const IndexEntry& index,
                                                          const CanonicalQuery& query,
                                                          const QueryPlannerParams& params,
                                                          int direction) {
        QuerySolutionNode* solnRoot = NULL;

        // Build an ixscan over the id index, use it, and return it.
        IndexScanNode* isn = new IndexScanNode();
        isn->indexKeyPattern = index.keyPattern;
        isn->indexIsMultiKey = index.multikey;
        isn->maxScan = query.getParsed().getMaxScan();
        isn->addKeyMetadata = query.getParsed().returnKey();

        IndexBoundsBuilder::allValuesBounds(index.keyPattern, &isn->bounds);

        if (-1 == direction) {
            QueryPlannerCommon::reverseScans(isn);
            isn->direction = -1;
        }

        MatchExpression* filter = query.root()->shallowClone();

        // If it's find({}) remove the no-op root.
        if (MatchExpression::AND == filter->matchType() && (0 == filter->numChildren())) {
            // XXX wasteful fix
            delete filter;
            solnRoot = isn;
        }
        else {
            // TODO: We may not need to do the fetch if the predicates in root are covered.  But
            // for now it's safe (though *maybe* slower).
            FetchNode* fetch = new FetchNode();
            fetch->filter.reset(filter);
            fetch->children.push_back(isn);
            solnRoot = fetch;
        }

        return solnRoot;
    }

    // static
    void QueryPlannerAccess::_addFilterToSolutionNode(QuerySolutionNode* node,
                                                      MatchExpression* match,
                                                      MatchExpression::MatchType type) {
        if (NULL == node->filter) {
            node->filter.reset(match);
        }
        else if (type == node->filter->matchType()) {
            // The 'node' already has either an AND or OR filter that matches 'type'. Add 'match' as
            // another branch of the filter.
            ListOfMatchExpression* listFilter =
                static_cast<ListOfMatchExpression*>(node->filter.get());
            listFilter->add(match);
        }
        else {
            // The 'node' already has a filter that does not match 'type'. If 'type' is AND, then
            // combine 'match' with the existing filter by adding an AND. If 'type' is OR, combine
            // by adding an OR node.
            ListOfMatchExpression* listFilter;
            if (MatchExpression::AND == type) {
                listFilter = new AndMatchExpression();
            }
            else {
                verify(MatchExpression::OR == type);
                listFilter = new OrMatchExpression();
            }
            MatchExpression* oldFilter = node->filter->shallowClone();
            listFilter->add(oldFilter);
            listFilter->add(match);
            node->filter.reset(listFilter);
        }
    }

    QuerySolutionNode* QueryPlannerAccess::makeIndexScan(const IndexEntry& index,
                                                         const CanonicalQuery& query,
                                                         const QueryPlannerParams& params,
                                                         const BSONObj& startKey,
                                                         const BSONObj& endKey) {
        QuerySolutionNode* solnRoot = NULL;

        // Build an ixscan over the id index, use it, and return it.
        IndexScanNode* isn = new IndexScanNode();
        isn->indexKeyPattern = index.keyPattern;
        isn->indexIsMultiKey = index.multikey;
        isn->direction = 1;
        isn->maxScan = query.getParsed().getMaxScan();
        isn->addKeyMetadata = query.getParsed().returnKey();
        isn->bounds.isSimpleRange = true;
        isn->bounds.startKey = startKey;
        isn->bounds.endKey = endKey;
        isn->bounds.endKeyInclusive = false;

        MatchExpression* filter = query.root()->shallowClone();

        // If it's find({}) remove the no-op root.
        if (MatchExpression::AND == filter->matchType() && (0 == filter->numChildren())) {
            // XXX wasteful fix
            delete filter;
            solnRoot = isn;
        }
        else {
            // TODO: We may not need to do the fetch if the predicates in root are covered.  But
            // for now it's safe (though *maybe* slower).
            FetchNode* fetch = new FetchNode();
            fetch->filter.reset(filter);
            fetch->children.push_back(isn);
            solnRoot = fetch;
        }

        return solnRoot;
    }

}  // namespace mongo
