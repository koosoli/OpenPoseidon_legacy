#pragma once

#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <cfloat>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>

namespace Poseidon
{
template <class Field>
struct AStarNode
{
	Field _field;
	AStarNode *_parent;
	float _g;
	float _f;
	int _open;

	AStarNode(const Field &field, AStarNode *parent, float g, float f)
		: _field(field)
	{
		_parent = parent;
		_g = g;
		_f = f;
		_open = 0;	// negative if not open, future use - index in OpenList 
	}

	// operator == for Field is required
	bool operator == (const AStarNode &with) const
	{return _field == with._field;}

	USE_FAST_ALLOCATOR;
};

#define ASTAR_TEMPLATE \
template \
< \
	class Field, \
	class CostFunction, class HeuristicFunction, \
	class Iterator, \
	class ClosedList, class OpenList \
>

#define ASTAR_ARGS Field, CostFunction, HeuristicFunction, Iterator, ClosedList, OpenList

ASTAR_TEMPLATE
class AStar
{
protected:
	typedef AStarNode<Field> Node;

	bool _done;
	Node *_found;
	Field _destination;
	float _limitCost;

	ClosedList _closed;
	OpenList _open;
	CostFunction _costFunction;
	HeuristicFunction _heuristicFunction;

public:
	AStar
	(
		const Field &start, const Field &destination,
		const CostFunction &cost, const HeuristicFunction &heuristic,
		float limitCost
	)
		: _destination(destination), _costFunction(cost), _heuristicFunction(heuristic)
	{
		_done = false;
		_found = nullptr;
		_limitCost = limitCost;
		Init(start);
	}

	bool IsDone() const {return _done;}
	bool IsFound() const {return _found != nullptr;}
	const Node *GetLastNode() const {return _found;}
	const Node *GetBestNode() const;
	// the node (open or expanded) with the smallest heuristic: nearest the goal
	const Node *GetNearestNode() const;

	// returns number of iterations really processed
	int Process(int maxIters, void *context = nullptr);

protected:
	void Init(const Field &start);

	void UpdateClosedList(Node *node, const Field &field, Node *parent, float g, float f);
};

ASTAR_TEMPLATE
const AStarNode<Field> *AStar<ASTAR_ARGS>::GetBestNode() const
{
	typedef typename ClosedList::Iterator ClosedIterator;
	
	// check open list
	const Node *best = _open.GetFirst();
	if (best) return best;

	// search in closed list
	float heur = FLT_MAX;
	for (ClosedIterator iterator(_closed); iterator; ++iterator)
	{
		const Node *node = *iterator;
		float h = node->_f - node->_g;
		if (h < heur)
		{
			best = node;
			heur = h;
		}
	}
	return best;
}

ASTAR_TEMPLATE
const AStarNode<Field> *AStar<ASTAR_ARGS>::GetNearestNode() const
{
	typedef typename ClosedList::Iterator ClosedIterator;

	// the closed list holds every node created, open ones included
	const Node *best = nullptr;
	float heur = FLT_MAX;
	for (ClosedIterator iterator(_closed); iterator; ++iterator)
	{
		const Node *node = *iterator;
		float h = node->_f - node->_g;
		if (h < heur)
		{
			best = node;
			heur = h;
		}
	}
	return best ? best : _open.GetFirst();
}

ASTAR_TEMPLATE
void AStar<ASTAR_ARGS>::Init(const Field &start)
{
	float h = _heuristicFunction(start, _destination);
	Node *node = new Node(start, nullptr, 0, h);
	_closed.Add(node);
	_open.Add(node);
}

ASTAR_TEMPLATE
int AStar<ASTAR_ARGS>::Process(int maxIters, void *context)
{
	for (int i=0; i<maxIters; i++)
	{
		Node *best = nullptr;
		
		// select best node from open list, remove it
		if (!_open.RemoveFirst(best))
		{
			// path does not exist
			_done = true;
			return i;
		}
		AI_ERROR(best);
		best->_open = -1;

		// check if destination is reached
		if (best->_field == _destination)
		{
			// path found
			_done = true;
			_found = best;
			return i;
		}

		// expand all neighbours
		for (Iterator iterator(best->_field, context); iterator; ++iterator)
		{
			Field neighbour = iterator;
			
			// ??? check for neighbour == best->_parent (performance testing needed)

			float g = best->_g + _costFunction(best->_field, neighbour);
			if (g >= _limitCost) continue;

			float h = _heuristicFunction(neighbour, _destination);
			float f = g + h;

			// check if neigbour was previously expanded
			Node *node = _closed[neighbour.GetKey()];
			if (!node)
			{
				// new node
				Node *node = new Node(neighbour, best, g, f);
				_closed.Add(node);
				_open.Add(node);
			}
			else if (f < node->_f)
			{
				// node is found and update is needed
				if (node->_open >= 0)
				{
					// found on open list
					node->_field = neighbour; // update additional info
					node->_parent = best;
					node->_g = g;
					node->_f = f;
					_open.UpdateUp(node);
				}
				else
				{
					// found on closed list
					// DO NOT PERFORM THIS
					// result path is better cca by 1%, spent time is double
					// UpdateClosedList(node, neighbour, best, g, f);
				}
			}
		}
	}
	// path not found before maxIters iterations
	return maxIters;
}


ASTAR_TEMPLATE
void AStar<ASTAR_ARGS>::UpdateClosedList(Node *node, const Field &field, Node *parent, float g, float f)
{
	// update node
	node->_field = field; // update additional info
	node->_parent = parent;
	node->_g = g;
	node->_f = f;

	// add node to stack
	AUTO_STATIC_ARRAY(Node *, stack, 256);
	stack.Add(node);

	while (int n = stack.Size() > 0)
	{
		// pop node from stack
		Node *parent = stack[n - 1];
		stack.Delete(n - 1, 1);

		for (Iterator iterator(parent->_field); iterator; ++iterator)
		{
			// child node
			Field field = iterator;
			Node *kid = _closed[field.GetKey()];
			if (!kid) continue;

			float g = parent->_g + _costFunction(parent->_field, field);
			if (g < kid->_g)
			{
				// update child node
				kid->_field = field; // update additional info
				kid->_parent = parent;
				float h = kid->_f - kid->_g;
				kid->_g = g;
				kid->_f = g + h;

				if (kid->_open >= 0)
				{
					_open.UpdateUp(kid);
				}
				else
				{
					// add child node to stack
					stack.Add(kid);
				}
			}
		}
	}
}

}  // namespace Poseidon
