#MongoDB + Pub/Sub

Welcome to [MongoDB](https://github.com/mongodb/mongo)! This is an implementation of publish/subscribe on top of MongoDB v2.6.3 using [ZeroMQ](http://zeromq.org). A MongoDB summer 2014 intern project by [Alex Grover](https://github.com/ajgrover) and [Esha Maharishi](https://github.com/EshaMaharishi).

Note: this is a prototype and is _not_ production ready.

##Building

See docs/building.md or navigate to www.mongodb.org and search for "Building".

##Drivers

An example node.js driver is available [here](https://github.com/ajgrover/node-mongodb-pubsub). This driver provides access to all the additional functionality implemented here.

##Design

- why ZeroMQ?
- design considerations
- where we are today

##Features

- filters/projections
- database event notifications

##API Documentation

In addition to MongoDB's basic behavior, we implemented 4 additional database commands: `publish`, `subscribe`, `poll`, and `unsubscribe`.

These are accessible from the Mongo shell through the `ps` variable.

```javascript
> var ps = db.PS()
> var subscription = ps.subscribe(channel, [filter], [projection]) // returns a Subscription object
> ps.publish(channel, message)
> subscription.poll([timeout]) // returns message
> subscription.unsubscribe()
```

The server API for for each of these commands is as follows:

The API

###Publish

Signature:

```
{ publish : <channel>, message : <message> }
```

From the Mongo shell:

```
ps.publish(channel, message)
```

Arguments:

- `channel` Required. Must be a string.
- `message` Required. Must be a document.

Other:

- The channel `$events` is reserved for database event notifications and will return an error if a user attempts to publish to it.

###Subscribe

- document signature and behavior, errors, filters and projections
- document shell helper

####Subscriptions

- document subscription object methods
- document shell helper

####Database Events

- document channels and behavior, setParameter
- document shell helper

###Unsubscribe

- document signature and behavior, errors
- document shell helper

##Performance

- include graphs and numbers here

##TODO

- Use secure connections for internally propagating messages over ZMQ (Curve or SSL)
- Allow for exact matching channels rather than just ZMQ’s prefix matching
- Add secure access to subscriptions
- Internal system to allow synchronized on/off across cluster
- Only propagate messages internally to nodes who are subscribed
- Allow enable/disable of pubsub at runtime

##License
Most MongoDB source files (src/mongo folder and below) are made available under the terms of the GNU Affero General Public License (AGPL).  See individual files for details.

As an exception, the files in the client/, debian/, rpm/, utils/mongoutils, and all subdirectories thereof are made available under the terms of the Apache License, version 2.0.
