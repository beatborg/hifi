//
//  RenderableZoneEntityItem.h
//
//
//  Created by Clement on 4/22/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_RenderableZoneEntityItem_h
#define hifi_RenderableZoneEntityItem_h

#include <ZoneEntityItem.h>

class NetworkGeometry;

class RenderableZoneEntityItem : public ZoneEntityItem  {
public:
    static EntityItem* factory(const EntityItemID& entityID, const EntityItemProperties& properties);
    
    RenderableZoneEntityItem(const EntityItemID& entityItemID, const EntityItemProperties& properties) :
    ZoneEntityItem(entityItemID, properties)
    { }
    
    virtual bool setProperties(const EntityItemProperties& properties);
    virtual int readEntitySubclassDataFromBuffer(const unsigned char* data, int bytesLeftToRead,
                                                 ReadBitstreamToTreeParams& args,
                                                 EntityPropertyFlags& propertyFlags, bool overwriteLocalData);

    virtual bool contains(const glm::vec3& point);
    
private:
    
    QSharedPointer<NetworkGeometry> _compoundShapeModel;
};

#endif // hifi_RenderableZoneEntityItem_h